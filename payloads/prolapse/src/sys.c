#include "prolapse.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef PROLAPSE_HOST
#include <sched.h>
#include <sys/resource.h>
#endif

#define PROLAPSE_SYS_GETRLIMIT      194
#define PROLAPSE_SYS_SETRLIMIT      195
#define PROLAPSE_SYS_CLOCK_GETTIME  232
#define PROLAPSE_SYS_SCHED_YIELD    331
#define PROLAPSE_SYS_CPUSET_GET     487
#define PROLAPSE_SYS_CPUSET_SET     488
#define PROLAPSE_SYS_AIO_DELETE     662
#define PROLAPSE_SYS_AIO_POLL       664
#define PROLAPSE_SYS_AIO_CANCEL     666
#define PROLAPSE_SYS_AIO_SUBMIT     669
#define PROLAPSE_RLIMIT_NOFILE      8
#define PROLAPSE_CLOCK_MONOTONIC    4

#define PROLAPSE_AIO_CMD_READ  0x001u
#define PROLAPSE_AIO_CMD_MULTI 0x1000u
#define PROLAPSE_AIO_PRIO_HIGH 3u

static uint64_t g_sys_clock;
static uint64_t g_sys_vm;
static uint64_t g_sys_aio_submit;
static uint64_t g_sys_aio_delete;
static uint64_t g_sys_aio_poll;
static uint64_t g_sys_aio_cancel;

int prolapse_sys_bind(const prolapse_config_t *cfg)
{
    if (prolapse_config_get_u64(cfg, "sys.clock_gettime", &g_sys_clock) != 0 ||
        prolapse_config_get_u64(cfg, "sys.vm_counter", &g_sys_vm) != 0 ||
        prolapse_config_get_u64(cfg, "sys.aio_submit_cmd",
                                &g_sys_aio_submit) != 0 ||
        prolapse_config_get_u64(cfg, "sys.aio_multi_delete",
                                &g_sys_aio_delete) != 0 ||
        prolapse_config_get_u64(cfg, "sys.aio_multi_poll",
                                &g_sys_aio_poll) != 0 ||
        prolapse_config_get_u64(cfg, "sys.aio_multi_cancel",
                                &g_sys_aio_cancel) != 0 ||
        g_sys_clock == 0 || g_sys_vm == 0 || g_sys_aio_submit == 0 ||
        g_sys_aio_delete == 0 || g_sys_aio_poll == 0 ||
        g_sys_aio_cancel == 0) {
        fprintf(stderr, "prolapse: ledger is missing a syscall number\n");
        return -1;
    }
    printf("[config] syscalls clock=%llu vm=%llu submit=%llu delete=%llu "
           "poll=%llu cancel=%llu "
           "(14.00 verify pending; use --set to override)\n",
           (unsigned long long)g_sys_clock, (unsigned long long)g_sys_vm,
           (unsigned long long)g_sys_aio_submit,
           (unsigned long long)g_sys_aio_delete,
           (unsigned long long)g_sys_aio_poll,
           (unsigned long long)g_sys_aio_cancel);
    return 0;
}

typedef struct {
    int64_t offset;
    uint64_t nbytes;
    void *buf;
    void *unk;
    int32_t fd;
    int32_t pad;
} prolapse_aio_rw_request_t;

typedef struct {
    uint64_t rlim_cur;
    uint64_t rlim_max;
} prolapse_rlimit_t;

#ifndef PROLAPSE_HOST
static long raw6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    unsigned char cf = 0;

    __asm__ __volatile__("syscall\n\tsetc %1\n\t"
        : "=a"(ret), "=r"(cf)
        : "0"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return cf ? -ret : ret;
}
#endif

static int raw_clock(struct timespec *ts)
{
#ifdef PROLAPSE_HOST
    return clock_gettime(CLOCK_MONOTONIC, ts);
#else
    if (g_sys_clock == 0)
        return -1;
    return (int)raw6((long)g_sys_clock, PROLAPSE_CLOCK_MONOTONIC,
                     (long)ts, 0, 0, 0, 0);
#endif
}

uint64_t prolapse_now_us(void)
{
    struct timespec ts;
    if (raw_clock(&ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

uint64_t prolapse_vm_counter(void)
{
#ifdef PROLAPSE_HOST
    return 0;
#else
    if (g_sys_vm == 0)
        return 0;
    return (uint64_t)raw6((long)g_sys_vm, 0, 0, 0, 0, 0, 0);
#endif
}

int prolapse_pin_cpu(int cpu)
{
#ifdef PROLAPSE_HOST
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
#else
    uint64_t mask = 1ull << (unsigned)cpu;
    return (int)raw6(PROLAPSE_SYS_CPUSET_SET, 3, 1, -1, (long)sizeof(mask),
                     (long)&mask, 0);
#endif
}

int prolapse_get_affinity(uint64_t *mask)
{
#ifdef PROLAPSE_HOST
    *mask = 0x3ull;
    return 0;
#else
    uint64_t m = 0;
    int ret = (int)raw6(PROLAPSE_SYS_CPUSET_GET, 3, 1, -1,
                        (long)sizeof(m), (long)&m, 0);
    if (ret == 0 && mask)
        *mask = m;
    return ret;
#endif
}

void prolapse_sleep_us(uint64_t us)
{
    uint64_t start = prolapse_now_us();
    uint64_t target;
    if (start == 0)
        return;
    target = start + us;
    for (;;) {
        uint64_t now = prolapse_now_us();
        if (now == 0 || now >= target)
            return;
#ifndef PROLAPSE_HOST
        raw6(PROLAPSE_SYS_SCHED_YIELD, 0, 0, 0, 0, 0, 0);
#else
        sched_yield();
#endif
    }
}

int prolapse_preflight_clock(char *why, size_t why_len)
{
    struct timespec a, b;
    int r1, r2;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    r1 = raw_clock(&a);
    prolapse_sleep_us(2000);
    r2 = raw_clock(&b);
    if (r1 != 0 || r2 != 0) {
        snprintf(why, why_len,
                 "clock_gettime(232) failed: r1=%d r2=%d", r1, r2);
        return -1;
    }
    if (b.tv_sec < a.tv_sec ||
        (b.tv_sec == a.tv_sec && b.tv_nsec <= a.tv_nsec)) {
        snprintf(why, why_len,
                 "clock_gettime(232) did not advance: %lld.%09ld -> %lld.%09ld",
                 (long long)a.tv_sec, a.tv_nsec,
                 (long long)b.tv_sec, b.tv_nsec);
        return -1;
    }
    printf("[preflight] clock_gettime(232): ret=0 %lld.%09ld -> %lld.%09ld\n",
           (long long)a.tv_sec, a.tv_nsec, (long long)b.tv_sec, b.tv_nsec);
    return 0;
}

int prolapse_preflight_cpuset(int *cpu_a, int *cpu_b, char *why,
                              size_t why_len)
{
    uint64_t mask = 0;
    int cpus[64];
    int n = 0;
    int i;
    int ret = prolapse_get_affinity(&mask);

    if (ret != 0) {
        snprintf(why, why_len, "cpuset_getaffinity(487) failed: ret=%d", ret);
        return -1;
    }
    for (i = 0; i < 64; i++) {
        if (mask & (1ull << (unsigned)i))
            cpus[n++] = i;
    }
    if (n < 2) {
        snprintf(why, why_len,
                 "cpuset_getaffinity returned %d allowed CPU(s), need >=2 "
                 "(mask=0x%llx)", n, (unsigned long long)mask);
        return -1;
    }
    *cpu_a = cpus[0];
    *cpu_b = cpus[1];
    printf("[preflight] cpuset_getaffinity(3,1,-1): mask=0x%llx cpus=%d "
           "pick=%d,%d\n", (unsigned long long)mask, n, *cpu_a, *cpu_b);
    return 0;
}

int prolapse_preflight_rlimit(uint64_t needed, char *why, size_t why_len)
{
    prolapse_rlimit_t rl;
    memset(&rl, 0, sizeof(rl));

#ifdef PROLAPSE_HOST
    {
        struct rlimit host_rl;
        if (getrlimit(RLIMIT_NOFILE, &host_rl) != 0) {
            snprintf(why, why_len, "getrlimit failed: %s", strerror(errno));
            return -1;
        }
        rl.rlim_cur = (uint64_t)host_rl.rlim_cur;
        rl.rlim_max = (uint64_t)host_rl.rlim_max;
    }
#else
    if (raw6(PROLAPSE_SYS_GETRLIMIT, PROLAPSE_RLIMIT_NOFILE, (long)&rl,
             0, 0, 0, 0) != 0) {
        snprintf(why, why_len, "getrlimit(194) failed");
        return -1;
    }
#endif
    if (rl.rlim_cur < needed) {
        if (rl.rlim_max < needed) {
            snprintf(why, why_len,
                     "rlimit max %llu < needed %llu; raise refused",
                     (unsigned long long)rl.rlim_max,
                     (unsigned long long)needed);
            return -1;
        }
        rl.rlim_cur = needed;
#ifdef PROLAPSE_HOST
        {
            struct rlimit host_rl;
            host_rl.rlim_cur = (rlim_t)needed;
            host_rl.rlim_max = (rlim_t)rl.rlim_max;
            if (setrlimit(RLIMIT_NOFILE, &host_rl) != 0) {
                snprintf(why, why_len, "setrlimit failed: %s",
                         strerror(errno));
                return -1;
            }
        }
#else
        if (raw6(PROLAPSE_SYS_SETRLIMIT, PROLAPSE_RLIMIT_NOFILE, (long)&rl,
                 0, 0, 0, 0) != 0) {
            snprintf(why, why_len, "setrlimit(195) refused");
            return -1;
        }
#endif
    }
    printf("[preflight] rlimit NOFILE: %llu -> %llu (needed %llu)\n",
           (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max,
           (unsigned long long)needed);
    return 0;
}

long prolapse_aio_submit_read(const int *fds, uint32_t n, void *buf,
                              prolapse_aio_id_t *ids, int *errs)
{
    prolapse_aio_rw_request_t *req;
    uint32_t i;
    long ret;

    if (!fds || !ids || !buf || n == 0 || n > PROLAPSE_MAX_BATCH) {
        if (errs)
            errs[0] = EINVAL;
        return -1;
    }
    req = (prolapse_aio_rw_request_t *)calloc(n, sizeof(*req));
    if (!req) {
        if (errs)
            errs[0] = ENOMEM;
        return -1;
    }
    for (i = 0; i < n; i++) {
        req[i].offset = 0;
        req[i].nbytes = 0x1000;
        req[i].buf = buf;
        req[i].unk = NULL;
        req[i].fd = fds[i];
        req[i].pad = 0;
    }
#ifdef PROLAPSE_HOST
    (void)ret;
    if (errs)
        errs[0] = ENOSYS;
    free(req);
    return -1;
#else
    if (g_sys_aio_submit == 0 || g_sys_aio_delete == 0 ||
        g_sys_aio_poll == 0 || g_sys_aio_cancel == 0) {
        if (errs)
            errs[0] = ENOSYS;
        free(req);
        return -1;
    }
    ret = raw6((long)g_sys_aio_submit,
               PROLAPSE_AIO_CMD_READ | PROLAPSE_AIO_CMD_MULTI,
               (long)req, n, PROLAPSE_AIO_PRIO_HIGH, (long)ids, 0);
    free(req);
    return ret;
#endif
}

long prolapse_aio_cancel(const prolapse_aio_id_t *ids, uint32_t n,
                         uint32_t *states)
{
#ifdef PROLAPSE_HOST
    (void)ids; (void)n; (void)states; errno = ENOSYS; return -1;
#else
    if (g_sys_aio_cancel == 0) {
        errno = ENOSYS;
        return -1;
    }
    return raw6((long)g_sys_aio_cancel, (long)ids, n, (long)states,
                0, 0, 0);
#endif
}

long prolapse_aio_poll(const prolapse_aio_id_t *ids, uint32_t n,
                       uint32_t *states)
{
#ifdef PROLAPSE_HOST
    (void)ids; (void)n; (void)states; errno = ENOSYS; return -1;
#else
    if (g_sys_aio_poll == 0) {
        errno = ENOSYS;
        return -1;
    }
    return raw6((long)g_sys_aio_poll, (long)ids, n, (long)states,
                0, 0, 0);
#endif
}

long prolapse_aio_delete(const prolapse_aio_id_t *ids, uint32_t n, int *errs)
{
#ifdef PROLAPSE_HOST
    (void)ids; (void)n; (void)errs; errno = ENOSYS; return -1;
#else
    if (g_sys_aio_delete == 0) {
        errno = ENOSYS;
        return -1;
    }
    return raw6((long)g_sys_aio_delete, (long)ids, n, (long)errs,
                0, 0, 0);
#endif
}
