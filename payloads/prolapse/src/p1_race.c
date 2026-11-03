#include "prolapse.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define P1_MAX_WORKERS PROLAPSE_MAX_THREADS
#define P1_MAX_WINDOW_SAMPLES 2000000u

typedef struct {
    volatile int go;
    volatile int done;
    volatile int entered;
    int pin_ret;
    uint32_t n;
    prolapse_aio_id_t ids[PROLAPSE_MAX_BATCH];
    int errs[PROLAPSE_MAX_BATCH];
    int peers[PROLAPSE_MAX_BATCH];
    int peer_count;
    long ret;
    uint64_t t_start_us;
    uint64_t t_end_us;
    uint64_t s624_in;
    uint64_t s624_out;
    int seq;
} p1_job_t;

struct p1_worker {
    pthread_t tid;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    p1_job_t job;
    p1_job_t *job_ptr;
    int stop;
    int ready;
    int active;
    int parked;
    int cpu;
    int index;
};

typedef struct {
    struct p1_worker workers[P1_MAX_WORKERS];
    int count;
    int max_threads;
    int parked_live;
    uint64_t cap_hits;
} p1_pool_t;

typedef struct {
    uint32_t n;
    prolapse_aio_id_t ids[PROLAPSE_MAX_BATCH];
    int peers[PROLAPSE_MAX_BATCH];
} p1_round_t;

typedef struct {
    const char *name;
    uint64_t attempts;
    uint64_t wins;
    uint64_t esrch_main;
    uint64_t esrch_racer;
    uint64_t both_esrch;
    uint64_t timeouts;
    uint64_t other;
    uint64_t cap_hits;
    uint64_t *windows;
    size_t window_count;
    size_t window_cap;
    uint64_t dropped_windows;
    uint64_t p50;
    uint64_t p90;
    uint64_t p99;
} p1_stats_t;

static int g_call_seq;
static uint64_t g_round_counter;
static uint64_t g_attempt_seq;
static char g_aio_buf[0x1000];

static void *worker_main(void *arg)
{
    struct p1_worker *w = (struct p1_worker *)arg;
    __atomic_store_n(&w->ready, 1, __ATOMIC_RELEASE);

    for (;;) {
        p1_job_t *job;
        pthread_mutex_lock(&w->mu);
        while (!w->stop && !w->job_ptr)
            pthread_cond_wait(&w->cv, &w->mu);
        if (w->stop && !w->job_ptr) {
            pthread_mutex_unlock(&w->mu);
            break;
        }
        job = w->job_ptr;
        pthread_mutex_unlock(&w->mu);

        while (!__atomic_load_n(&job->go, __ATOMIC_ACQUIRE)) {
            if (w->stop)
                break;
            sched_yield();
        }
        if (w->stop) {
            __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
        } else {
            job->pin_ret = prolapse_pin_cpu(w->cpu);
            if (job->pin_ret != 0) {
                uint32_t i;
                job->ret = -1;
                for (i = 0; i < job->n; i++)
                    job->errs[i] = EPERM;
            } else {
                __atomic_store_n(&job->entered, 1, __ATOMIC_RELEASE);
                job->s624_in = prolapse_vm_counter();
                job->t_start_us = prolapse_now_us();
                job->ret = prolapse_aio_delete(job->ids, job->n, job->errs);
                job->t_end_us = prolapse_now_us();
                job->s624_out = prolapse_vm_counter();
            }
            job->seq = __sync_add_and_fetch(&g_call_seq, 1);
            __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
        }

        pthread_mutex_lock(&w->mu);
        w->job_ptr = NULL;
        __atomic_store_n(&w->active, 0, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&w->cv);
        pthread_mutex_unlock(&w->mu);
    }
    return NULL;
}

static int pool_spawn(p1_pool_t *pool, int cpu)
{
    struct p1_worker *w;
    if (pool->count >= pool->max_threads)
        return -1;
    w = &pool->workers[pool->count];
    memset(w, 0, sizeof(*w));
    w->index = pool->count;
    w->cpu = cpu;
    pthread_mutex_init(&w->mu, NULL);
    pthread_cond_init(&w->cv, NULL);
    if (pthread_create(&w->tid, NULL, worker_main, w) != 0) {
        pthread_mutex_destroy(&w->mu);
        pthread_cond_destroy(&w->cv);
        return -1;
    }
    pool->count++;
    while (!__atomic_load_n(&w->ready, __ATOMIC_ACQUIRE))
        sched_yield();
    return 0;
}

static int pool_assign(p1_pool_t *pool, int cpu, p1_job_t **job_out)
{
    int i;
    for (;;) {
        for (i = 0; i < pool->count; i++) {
            struct p1_worker *w = &pool->workers[i];
            if (__atomic_load_n(&w->active, __ATOMIC_ACQUIRE) || w->parked)
                continue;
            {
                p1_job_t *job = &w->job;
                memset(job, 0, sizeof(*job));
                w->cpu = cpu;
                pthread_mutex_lock(&w->mu);
                w->job_ptr = job;
                __atomic_store_n(&w->active, 1, __ATOMIC_RELEASE);
                pthread_cond_signal(&w->cv);
                pthread_mutex_unlock(&w->mu);
                *job_out = job;
            }
            return i;
        }
        if (pool->count < pool->max_threads) {
            if (pool_spawn(pool, cpu) == 0)
                continue;
        }
        pool->cap_hits++;
        return -1;
    }
}

static int pool_wait_done(const p1_job_t *job, uint64_t timeout_us)
{
    uint64_t start = prolapse_now_us();
    if (start == 0)
        return -1;
    while (!__atomic_load_n(&job->done, __ATOMIC_ACQUIRE)) {
        uint64_t now = prolapse_now_us();
        if (now == 0 || now - start >= timeout_us)
            return 0;
        sched_yield();
    }
    return 1;
}

static int pool_wait_entered(const p1_job_t *job, uint64_t timeout_us)
{
    uint64_t start = prolapse_now_us();
    if (start == 0)
        return -1;
    while (!__atomic_load_n(&job->entered, __ATOMIC_ACQUIRE)) {
        uint64_t now;
        if (__atomic_load_n(&job->done, __ATOMIC_ACQUIRE))
            return 1;
        now = prolapse_now_us();
        if (now == 0 || now - start >= timeout_us)
            return 0;
        sched_yield();
    }
    return 1;
}

static int pool_wait_idle(p1_pool_t *pool, int index, uint64_t timeout_us)
{
    uint64_t start;
    if (index < 0 || index >= pool->count)
        return -1;
    start = prolapse_now_us();
    if (start == 0)
        return -1;
    while (__atomic_load_n(&pool->workers[index].active, __ATOMIC_ACQUIRE)) {
        uint64_t now = prolapse_now_us();
        if (now == 0 || now - start >= timeout_us)
            return 0;
        sched_yield();
    }
    return 1;
}

static void pool_mark_parked(p1_pool_t *pool, int index)
{
    if (index < 0 || index >= pool->count)
        return;
    if (!pool->workers[index].parked) {
        pool->workers[index].parked = 1;
        pool->parked_live++;
    }
}

static int pool_release_parked(p1_pool_t *pool, int index, p1_job_t *job,
                               uint64_t park_timeout_us, char *why,
                               size_t why_len)
{
    uint32_t i;
    int drained;
    if (index < 0 || index >= pool->count)
        return -1;
    for (i = 0; i < (uint32_t)job->peer_count; i++) {
        if (job->peers[i] >= 0) {
            if (prolapse_transport_release_peer(&job->peers[i], why,
                                                why_len) != 0)
                return -1;
        }
    }
    drained = pool_wait_done(job, park_timeout_us);
    if (drained != 1) {
        snprintf(why, why_len,
                 "parked worker %d did not return after SO_LINGER {0,0} "
                 "release (waited %llu us)",
                 index, (unsigned long long)park_timeout_us);
        return -1;
    }
    if (pool_wait_idle(pool, index, 1000000) != 1) {
        snprintf(why, why_len, "parked worker %d did not become idle", index);
        return -1;
    }
    if (pool->workers[index].parked) {
        pool->workers[index].parked = 0;
        if (pool->parked_live > 0)
            pool->parked_live--;
    }
    return 0;
}

static int pool_drain(p1_pool_t *pool, uint64_t park_timeout_us, char *why,
                      size_t why_len)
{
    int i;
    for (i = 0; i < pool->count; i++) {
        struct p1_worker *w = &pool->workers[i];
        if (w->parked) {
            if (pool_release_parked(pool, i, &w->job, park_timeout_us,
                                    why, why_len) != 0)
                return -1;
        }
        if (pool_wait_idle(pool, i, 1000000) != 1) {
            snprintf(why, why_len, "worker %d still active at drain", i);
            return -1;
        }
    }
    if (pool->parked_live != 0) {
        snprintf(why, why_len, "parked_live=%d at drain, expected 0",
                 pool->parked_live);
        return -1;
    }
    for (i = 0; i < pool->count; i++) {
        struct p1_worker *w = &pool->workers[i];
        pthread_mutex_lock(&w->mu);
        w->stop = 1;
        pthread_cond_broadcast(&w->cv);
        pthread_mutex_unlock(&w->mu);
    }
    for (i = 0; i < pool->count; i++) {
        pthread_join(pool->workers[i].tid, NULL);
        pthread_mutex_destroy(&pool->workers[i].mu);
        pthread_cond_destroy(&pool->workers[i].cv);
    }
    printf("[P1] parked_live=0 drain=ok threads=%d cap_hits=%llu\n",
           pool->count, (unsigned long long)pool->cap_hits);
    return 0;
}

static void round_close_peers(p1_round_t *r);

static int prepare_round(p1_round_t *r, uint32_t n, const prolapse_config_t *cfg,
                         char *why, size_t why_len)
{
    prolapse_transport_t tr[PROLAPSE_MAX_BATCH];
    int fds[PROLAPSE_MAX_BATCH];
    uint32_t states[PROLAPSE_MAX_BATCH];
    uint32_t i;
    const char *kind = prolapse_config_get(cfg, "race.transport");
    int linger_onoff = 1, linger_seconds = 1;
    uint32_t terminal_timeout;
    uint64_t start;
    long ret;

    (void)prolapse_config_get_int(cfg, "linger.onoff", &linger_onoff);
    (void)prolapse_config_get_int(cfg, "linger.seconds", &linger_seconds);
    (void)prolapse_config_get_u32(cfg, "race.terminal_timeout_us",
                                  &terminal_timeout);
    if (n == 0 || n > PROLAPSE_MAX_BATCH) {
        snprintf(why, why_len, "bad batch %u", n);
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->n = n;
    for (i = 0; i < n; i++)
        r->peers[i] = -1;

    for (i = 0; i < n; i++) {
        memset(&tr[i], 0, sizeof(tr[i]));
        tr[i].target = -1;
        tr[i].peer = -1;
        if (prolapse_transport_open(&tr[i], kind, linger_onoff, linger_seconds,
                                    why, why_len) != 0) {
            uint32_t j;
            for (j = 0; j < i; j++)
                prolapse_transport_close(&tr[j]);
            return -1;
        }
        fds[i] = tr[i].target;
    }

    ret = prolapse_aio_submit_read(fds, n, g_aio_buf, r->ids,
                                   (int *)states);
    if (ret != 0) {
        snprintf(why, why_len, "aio_submit_cmd(READ|MULTI) failed: ret=%ld",
                 ret);
        for (i = 0; i < n; i++)
            prolapse_transport_close(&tr[i]);
        return -1;
    }

    /* The AIO request owns the target fd now.  Keep only the peer. */
    for (i = 0; i < n; i++) {
        close(tr[i].target);
        tr[i].target = -1;
        r->peers[i] = tr[i].peer;
        tr[i].peer = -1;
        states[i] = 0;
    }

    (void)prolapse_aio_cancel(r->ids, n, states);
    start = prolapse_now_us();
    if (start == 0) {
        snprintf(why, why_len, "clock_gettime(232) failed after submit");
        return -1;
    }
    for (;;) {
        int all_terminal = 1;
        (void)prolapse_aio_poll(r->ids, n, states);
        for (i = 0; i < n; i++) {
            if (states[i] != 3u && states[i] != 4u)
                all_terminal = 0;
        }
        if (all_terminal)
            break;
        if (prolapse_now_us() - start >= terminal_timeout) {
            snprintf(why, why_len,
                     "aio_poll did not reach terminal state within %u us",
                     terminal_timeout);
            round_close_peers(r);
            return -1;
        }
        prolapse_sleep_us(100);
    }
    return 0;
}

static void round_close_peers(p1_round_t *r)
{
    uint32_t i;
    for (i = 0; i < r->n; i++) {
        if (r->peers[i] >= 0) {
            close(r->peers[i]);
            r->peers[i] = -1;
        }
    }
}

static int one_delete(p1_pool_t *pool, int cpu, prolapse_aio_id_t id,
                      int *peer_fd, uint64_t wait_us, int *err, long *ret,
                      int *timed_out, char *why, size_t why_len)
{
    p1_job_t *job = NULL;
    int index = pool_assign(pool, cpu, &job);
    int waited;
    if (index < 0) {
        snprintf(why, why_len, "thread cap reached while assigning control");
        return -1;
    }
    job->n = 1;
    job->ids[0] = id;
    job->peer_count = 1;
    job->peers[0] = *peer_fd;
    __atomic_store_n(&job->go, 1, __ATOMIC_RELEASE);
    waited = pool_wait_done(job, wait_us);
    if (waited == 0) {
        *timed_out = 1;
        *err = INT_MIN;
        *ret = -1;
        pool_mark_parked(pool, index);
        printf("[P1] control delete parked id=0x%08x; releasing\n",
               (unsigned)id);
        if (pool_release_parked(pool, index, job, wait_us, why, why_len) != 0)
            return -1;
        *peer_fd = -1;
        return 0;
    }
    if (waited < 0) {
        snprintf(why, why_len, "clock failed while waiting for control delete");
        return -1;
    }
    if (pool_wait_idle(pool, index, 1000000) != 1) {
        snprintf(why, why_len, "control worker %d did not become idle", index);
        return -1;
    }
    *timed_out = 0;
    *err = job->errs[0];
    *ret = job->ret;
    if (job->pin_ret != 0) {
        snprintf(why, why_len, "worker pin failed: %d", job->pin_ret);
        return -1;
    }
    return 0;
}

static uint32_t bump_generation(prolapse_aio_id_t id)
{
    uint32_t gen = (id >> 13) & 7u;
    gen = (gen + 1u) & 7u;
    return (id & ~0xE000u) | (gen << 13);
}

static int serialized_control(p1_pool_t *pool, const prolapse_config_t *cfg,
                              int cpu, FILE *out, char *why, size_t why_len)
{
    uint32_t rounds = 0;
    uint32_t park_timeout = 2000000;
    uint64_t both_zero = 0;
    uint32_t i;
    (void)prolapse_config_get_u32(cfg, "race.control_rounds", &rounds);
    (void)prolapse_config_get_u32(cfg, "linger.park_timeout_us",
                                  &park_timeout);

    for (i = 0; i < rounds; i++) {
        p1_round_t r;
        int err1 = INT_MIN, err2 = INT_MIN;
        long ret1 = -1, ret2 = -1;
        int timed1 = 0, timed2 = 0;
        int peer = -1;
        if (prepare_round(&r, 1, cfg, why, why_len) != 0)
            return -1;
        peer = r.peers[0];
        if (one_delete(pool, cpu, r.ids[0], &peer, park_timeout, &err1, &ret1,
                       &timed1, why, why_len) != 0) {
            round_close_peers(&r);
            return -1;
        }
        if (!timed1) {
            if (one_delete(pool, cpu, r.ids[0], &peer, park_timeout, &err2,
                           &ret2, &timed2, why, why_len) != 0) {
                round_close_peers(&r);
                return -1;
            }
        }
        r.peers[0] = peer;
        round_close_peers(&r);
        if (err1 == 0 && err2 == 0)
            both_zero++;
        if (i < 8 || (i + 1) == rounds) {
            printf("[control] serialized %u/%u id=0x%08x err1=%d err2=%d "
                   "ret1=%ld ret2=%ld both_zero=%llu\n",
                   i + 1, rounds, (unsigned)r.ids[0], err1, err2, ret1, ret2,
                   (unsigned long long)both_zero);
        }
    }
    if (prolapse_write_control(out, "serialized", rounds, both_zero, 0,
                               both_zero == 0,
                               "single thread, two sequential deletes, same id")
        != 0)
        return -1;
    if (both_zero != 0) {
        snprintf(why, why_len,
                 "serialized control produced %llu both_zero, expected ~0",
                 (unsigned long long)both_zero);
        return -1;
    }
    printf("[control] serialized PASS attempts=%u both_zero=%llu\n", rounds,
           (unsigned long long)both_zero);
    return 0;
}

static int generation_control(p1_pool_t *pool, const prolapse_config_t *cfg,
                              int cpu, FILE *out, char *why, size_t why_len)
{
    uint32_t rounds = 0;
    uint32_t park_timeout = 2000000;
    uint64_t pass = 0;
    uint32_t i;
    (void)prolapse_config_get_u32(cfg, "race.control_rounds", &rounds);
    (void)prolapse_config_get_u32(cfg, "linger.park_timeout_us",
                                  &park_timeout);

    for (i = 0; i < rounds; i++) {
        p1_round_t r;
        uint32_t bumped;
        int bumped_err = INT_MIN, original_err = INT_MIN;
        long bumped_ret = -1, original_ret = -1;
        int timed = 0;
        int peer = -1;
        int ok;
        if (prepare_round(&r, 1, cfg, why, why_len) != 0)
            return -1;
        peer = r.peers[0];
        bumped = bump_generation(r.ids[0]);
        if (one_delete(pool, cpu, bumped, &peer, park_timeout, &bumped_err,
                       &bumped_ret, &timed, why, why_len) != 0) {
            round_close_peers(&r);
            return -1;
        }
        if (!timed) {
            int timed2 = 0;
            (void)one_delete(pool, cpu, r.ids[0], &peer, park_timeout,
                             &original_err, &original_ret, &timed2,
                             why, why_len);
        }
        r.peers[0] = peer;
        round_close_peers(&r);
        ok = prolapse_is_esrch(bumped_err);
        if (ok)
            pass++;
        if (i < 8 || (i + 1) == rounds) {
            printf("[control] generation %u/%u id=0x%08x bumped=0x%08x "
                   "bumped_err=0x%08x (%d) original_err=%d pass=%llu\n",
                   i + 1, rounds, (unsigned)r.ids[0], (unsigned)bumped,
                   (unsigned)bumped_err, bumped_err, original_err,
                   (unsigned long long)pass);
        }
    }
    if (prolapse_write_control(out, "generation", rounds, pass, rounds,
                               pass == rounds,
                               "bumped generation must yield 0x80020003") != 0)
        return -1;
    if (pass != rounds) {
        snprintf(why, why_len,
                 "generation control passed %llu/%u; loser did not return "
                 "0x80020003", (unsigned long long)pass, rounds);
        return -1;
    }
    printf("[control] generation PASS attempts=%u invalid=0x80020003\n",
           rounds);
    return 0;
}

static int parking_preflight(p1_pool_t *pool, int cpu,
                             const prolapse_config_t *cfg, char *why,
                             size_t why_len)
{
    p1_round_t r;
    p1_job_t *job = NULL;
    int index;
    uint64_t park_timeout = 2000000;
    int waited;

    (void)prolapse_config_get_u64(cfg, "linger.park_timeout_us", &park_timeout);
    if (prepare_round(&r, 1, cfg, why, why_len) != 0)
        return -1;
    index = pool_assign(pool, cpu, &job);
    if (index < 0) {
        snprintf(why, why_len, "thread cap reached in parking preflight");
        round_close_peers(&r);
        return -1;
    }
    job->n = 1;
    job->ids[0] = r.ids[0];
    job->peer_count = 1;
    job->peers[0] = r.peers[0];
    __atomic_store_n(&job->go, 1, __ATOMIC_RELEASE);

    waited = pool_wait_entered(job, 100000);
    if (waited != 1) {
        snprintf(why, why_len,
                 "parking preflight: delete did not enter within 100 ms");
        round_close_peers(&r);
        return -1;
    }
    prolapse_sleep_us(5000);
    if (__atomic_load_n(&job->done, __ATOMIC_ACQUIRE)) {
        snprintf(why, why_len,
                 "parking preflight: delete returned before 5 ms; SO_LINGER "
                 "{1,1} did not hold");
        round_close_peers(&r);
        return -1;
    }

    pool_mark_parked(pool, index);
    printf("[preflight] linger park: held_after_5ms=1 parked_live=%d "
           "releasing with {0,0}\n", pool->parked_live);
    if (prolapse_transport_release_peer(&job->peers[0], why, why_len) != 0) {
        round_close_peers(&r);
        return -1;
    }
    r.peers[0] = -1;
    waited = pool_wait_done(job, park_timeout);
    if (waited != 1) {
        snprintf(why, why_len,
                 "parking preflight: parked worker did not return after "
                 "SO_LINGER {0,0}");
        return -1;
    }
    if (pool_wait_idle(pool, index, 1000000) != 1) {
        snprintf(why, why_len,
                 "parking preflight: worker did not become idle after release");
        return -1;
    }
    pool->workers[index].parked = 0;
    if (pool->parked_live > 0)
        pool->parked_live--;
    printf("[preflight] linger park: released=1 parked_live=%d\n",
           pool->parked_live);
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void stats_init(p1_stats_t *s, const char *name, uint64_t rounds,
                       uint32_t batch)
{
    size_t cap;
    memset(s, 0, sizeof(*s));
    s->name = name;
    cap = (size_t)rounds * (size_t)batch;
    if (cap == 0)
        cap = 1;
    if (cap > P1_MAX_WINDOW_SAMPLES)
        cap = P1_MAX_WINDOW_SAMPLES;
    s->windows = (uint64_t *)calloc(cap, sizeof(uint64_t));
    s->window_cap = s->windows ? cap : 0;
}

static void stats_add_window(p1_stats_t *s, uint64_t window_us)
{
    if (s->window_count < s->window_cap) {
        s->windows[s->window_count++] = window_us;
    } else {
        s->dropped_windows++;
    }
}

static void stats_finish(p1_stats_t *s)
{
    if (!s->windows || s->window_count == 0)
        return;
    qsort(s->windows, s->window_count, sizeof(uint64_t), cmp_u64);
    s->p50 = s->windows[(s->window_count * 50) / 100];
    s->p90 = s->windows[(s->window_count * 90) / 100];
    s->p99 = s->windows[(s->window_count * 99) / 100];
    if (s->p99 == 0)
        s->p99 = s->windows[s->window_count - 1];
}

static int run_strategy(p1_pool_t *pool, prolapse_config_t *cfg,
                        const char *strategy, FILE *out, uint64_t run_id,
                        int cpu_a, int cpu_b, int *stop_all,
                        char *why, size_t why_len)
{
    p1_stats_t stats;
    uint32_t rounds = 0, batch = 1, loss_threshold = 1000, stagger_us = 0;
    uint32_t linger_park_timeout = 2000000;
    uint32_t wait_us = 1000000;
    int dead_end_written = 0;
    uint32_t round;

    (void)prolapse_config_get_u32(cfg, "race.rounds", &rounds);
    (void)prolapse_config_get_u32(cfg, "race.batch", &batch);
    (void)prolapse_config_get_u32(cfg, "race.loss_threshold", &loss_threshold);
    (void)prolapse_config_get_u32(cfg, "race.stagger_us", &stagger_us);
    (void)prolapse_config_get_u32(cfg, "linger.park_timeout_us",
                                  &linger_park_timeout);
    (void)prolapse_config_get_u32(cfg, "race.terminal_timeout_us", &wait_us);
    stats_init(&stats, strategy, rounds, batch);
    if (!stats.windows) {
        snprintf(why, why_len, "out of memory for window samples");
        return -1;
    }

    for (round = 0; round < rounds; round++) {
        p1_round_t r;
        p1_job_t *jm = NULL, *jr = NULL;
        int im, ir;
        int main_cpu, racer_cpu;
        int main_done, racer_done;
        uint32_t slot;
        uint64_t round_id;
        uint64_t t_start = 0, t_end = 0, window_us = 0;
        uint32_t wins_this_round = 0;
        int any_win = 0;
        prolapse_outcome_t outcomes[PROLAPSE_MAX_BATCH];
        int main_errs[PROLAPSE_MAX_BATCH];
        int racer_errs[PROLAPSE_MAX_BATCH];
        int main_rets[PROLAPSE_MAX_BATCH];
        int racer_rets[PROLAPSE_MAX_BATCH];
        int main_seqs[PROLAPSE_MAX_BATCH];
        int racer_seqs[PROLAPSE_MAX_BATCH];

        if (strcmp(strategy, "same_barrier") == 0) {
            main_cpu = racer_cpu = cpu_a;
        } else {
            main_cpu = cpu_a;
            racer_cpu = cpu_b;
        }
        if (prepare_round(&r, batch, cfg, why, why_len) != 0)
            goto fail;

        im = pool_assign(pool, main_cpu, &jm);
        if (im < 0) {
            snprintf(why, why_len,
                     "thread cap reached: max_threads=%d active+parked=%d",
                     pool->max_threads, pool->count);
            round_close_peers(&r);
            goto cap_fail;
        }
        ir = pool_assign(pool, racer_cpu, &jr);
        if (ir < 0) {
            /* Let the already-assigned main worker retire before reporting
             * the cap; otherwise the drain would see a live worker. */
            jm->n = batch;
            jm->peer_count = (int)batch;
            for (slot = 0; slot < batch; slot++) {
                jm->ids[slot] = r.ids[slot];
                jm->peers[slot] = r.peers[slot];
            }
            __atomic_store_n(&jm->go, 1, __ATOMIC_RELEASE);
            {
                int md = pool_wait_done(jm, wait_us);
                if (md == 0) {
                    pool_mark_parked(pool, im);
                    if (pool_release_parked(pool, im, jm, linger_park_timeout,
                                            why, why_len) != 0) {
                        /* Release closes peers one at a time; adopt the
                         * job's table so we never re-close (fd reuse). */
                        for (slot = 0; slot < batch; slot++)
                            r.peers[slot] = jm->peers[slot];
                        round_close_peers(&r);
                        goto cap_fail;
                    }
                    for (slot = 0; slot < batch; slot++)
                        r.peers[slot] = -1;
                } else if (md == 1) {
                    (void)pool_wait_idle(pool, im, 1000000);
                }
            }
            snprintf(why, why_len,
                     "thread cap reached: max_threads=%d active+parked=%d",
                     pool->max_threads, pool->count);
            round_close_peers(&r);
            goto cap_fail;
        }

        for (slot = 0; slot < batch; slot++) {
            jm->ids[slot] = r.ids[slot];
            jr->ids[slot] = r.ids[slot];
            jm->peers[slot] = r.peers[slot];
            jr->peers[slot] = r.peers[slot];
        }
        jm->n = jr->n = batch;
        jm->peer_count = jr->peer_count = (int)batch;

        __atomic_store_n(&jm->go, 1, __ATOMIC_RELEASE);
        if (strcmp(strategy, "split_stagger") == 0 && stagger_us > 0)
            prolapse_sleep_us(stagger_us);
        __atomic_store_n(&jr->go, 1, __ATOMIC_RELEASE);

        main_done = pool_wait_done(jm, wait_us);
        racer_done = pool_wait_done(jr, wait_us);
        if (main_done == 0)
            pool_mark_parked(pool, im);
        if (racer_done == 0)
            pool_mark_parked(pool, ir);

        g_round_counter++;
        round_id = run_id ^ (g_round_counter * 0x9E3779B97F4A7C15ull);
        if (jm->t_start_us && jr->t_start_us)
            t_start = jm->t_start_us < jr->t_start_us ? jm->t_start_us
                                                      : jr->t_start_us;
        else
            t_start = jm->t_start_us ? jm->t_start_us : jr->t_start_us;
        if (main_done == 1 && racer_done == 1) {
            t_end = jm->t_end_us > jr->t_end_us ? jm->t_end_us : jr->t_end_us;
        } else {
            t_end = prolapse_now_us();
        }
        if (t_end > t_start)
            window_us = t_end - t_start;

        for (slot = 0; slot < batch; slot++) {
            main_rets[slot] = (main_done == 1) ? (int)jm->ret : -1;
            racer_rets[slot] = (racer_done == 1) ? (int)jr->ret : -1;
            main_errs[slot] = (main_done == 1) ? jm->errs[slot] : INT_MIN;
            racer_errs[slot] = (racer_done == 1) ? jr->errs[slot] : INT_MIN;
            main_seqs[slot] = (main_done == 1) ? jm->seq : 0;
            racer_seqs[slot] = (racer_done == 1) ? jr->seq : 0;
            if (main_done != 1 || racer_done != 1) {
                outcomes[slot] = PROLAPSE_OUTCOME_TIMEOUT;
            } else {
                outcomes[slot] = prolapse_classify(
                    main_rets[slot], racer_rets[slot],
                    main_errs[slot], racer_errs[slot]);
            }
            if (outcomes[slot] == PROLAPSE_OUTCOME_BOTH_ZERO) {
                wins_this_round++;
                any_win = 1;
            }
        }

        for (slot = 0; slot < batch; slot++) {
            prolapse_attempt_record_t rec;
            memset(&rec, 0, sizeof(rec));
            rec.run_id = run_id;
            rec.seq = ++g_attempt_seq;
            rec.round = round + 1;
            rec.round_id = round_id;
            rec.strategy = strategy;
            rec.id = r.ids[slot];
            rec.slot = slot;
            rec.batch_size = batch;
            rec.t_start_us = t_start;
            rec.t_end_us = t_end;
            rec.window_us = window_us;
            rec.sys624_main_before = jm->s624_in;
            rec.sys624_main_after = jm->s624_out;
            rec.sys624_racer_before = jr->s624_in;
            rec.sys624_racer_after = jr->s624_out;
            rec.main_ret = main_rets[slot];
            rec.main_err = main_errs[slot];
            rec.racer_ret = racer_rets[slot];
            rec.racer_err = racer_errs[slot];
            rec.main_seq = main_seqs[slot];
            rec.racer_seq = racer_seqs[slot];
            rec.parked_live = pool->parked_live;
            rec.outcome = outcomes[slot];
            rec.frozen = any_win ? 1 : 0;
            rec.candidate = outcomes[slot] == PROLAPSE_OUTCOME_BOTH_ZERO;
            rec.control = 0;
            if (prolapse_write_attempt(out, &rec) != 0) {
                snprintf(why, why_len, "JSONL write failed");
                round_close_peers(&r);
                goto fail;
            }

            stats.attempts++;
            stats_add_window(&stats, window_us);
            switch (outcomes[slot]) {
            case PROLAPSE_OUTCOME_BOTH_ZERO: stats.wins++; break;
            case PROLAPSE_OUTCOME_ESRCH_MAIN: stats.esrch_main++; break;
            case PROLAPSE_OUTCOME_ESRCH_RACER: stats.esrch_racer++; break;
            case PROLAPSE_OUTCOME_BOTH_ESRCH: stats.both_esrch++; break;
            case PROLAPSE_OUTCOME_TIMEOUT: stats.timeouts++; break;
            default: stats.other++; break;
            }
        }

        if (any_win) {
            fprintf(out,
                    "{\"record\":\"frozen_round\",\"run_id\":%llu,"
                    "\"round_id\":%llu,\"strategy\":\"%s\",\"batch_size\":%u,"
                    "\"wins\":%u}\n",
                    (unsigned long long)run_id, (unsigned long long)round_id,
                    strategy, batch, wins_this_round);
            fflush(out);
        }

        printf("[P1] round=%u strategy=%s attempts=%llu wins=%llu "
               "parked_live=%d window_us=%llu outcomes(wins=%u)\n",
               round + 1, strategy, (unsigned long long)stats.attempts,
               (unsigned long long)stats.wins, pool->parked_live,
               (unsigned long long)window_us, wins_this_round);

        if (main_done == 1 && pool_wait_idle(pool, im, 1000000) != 1) {
            snprintf(why, why_len, "main worker did not become idle");
            round_close_peers(&r);
            goto fail;
        }
        if (racer_done == 1 && pool_wait_idle(pool, ir, 1000000) != 1) {
            snprintf(why, why_len, "racer worker did not become idle");
            round_close_peers(&r);
            goto fail;
        }
        if (main_done == 0) {
            if (pool_release_parked(pool, im, jm, linger_park_timeout,
                                    why, why_len) != 0) {
                for (slot = 0; slot < batch; slot++)
                    r.peers[slot] = jm->peers[slot];
                round_close_peers(&r);
                goto fail;
            }
            /* Releasing the first parked job closes the shared peer fds. */
            for (slot = 0; slot < batch; slot++) {
                r.peers[slot] = -1;
                jr->peers[slot] = -1;
            }
        }
        if (racer_done == 0) {
            if (pool_release_parked(pool, ir, jr, linger_park_timeout,
                                    why, why_len) != 0) {
                for (slot = 0; slot < batch; slot++)
                    r.peers[slot] = jr->peers[slot];
                round_close_peers(&r);
                goto fail;
            }
            for (slot = 0; slot < batch; slot++)
                r.peers[slot] = -1;
        }
        if (main_done == 1 && racer_done == 1)
            round_close_peers(&r);
        else
            for (slot = 0; slot < batch; slot++)
                r.peers[slot] = -1;

        {
            int stop_on_win = 1;
            (void)prolapse_config_get_int(cfg, "race.stop_on_first_win",
                                          &stop_on_win);
            if (any_win && stop_on_win) {
                printf("[P1] FROZEN first win strategy=%s round=%u "
                       "round_id=%llu\n", strategy, round + 1,
                       (unsigned long long)round_id);
                *stop_all = 1;
                break;
            }
        }

        if (stats.wins == 0 && stats.attempts >= loss_threshold &&
            !dead_end_written) {
            uint64_t p99 = 0;
            stats_finish(&stats);
            p99 = stats.p99;
            if (prolapse_append_dead_end(cfg->dead_ends_path, strategy,
                                         stats.attempts, stats.wins, p99,
                                         stats.cap_hits,
                                         "loss_threshold reached in-run") != 0) {
                snprintf(why, why_len, "dead-end append failed");
                goto fail;
            }
            dead_end_written = 1;
            printf("[P1] DEAD_END strategy=%s attempts=%llu wins=0 p99=%llu "
                   "appended=%s\n", strategy,
                   (unsigned long long)stats.attempts,
                   (unsigned long long)p99, cfg->dead_ends_path);
            break;
        }
        continue;

cap_fail:
        stats.cap_hits++;
        stats_finish(&stats);
        if (!dead_end_written) {
            (void)prolapse_append_dead_end(cfg->dead_ends_path, strategy,
                                           stats.attempts, stats.wins,
                                           stats.p99, stats.cap_hits,
                                           "thread cap tripped; run aborted");
            dead_end_written = 1;
        }
        goto fail;
    }

    stats_finish(&stats);
    if (stats.wins == 0 && !dead_end_written) {
        if (prolapse_append_dead_end(cfg->dead_ends_path, strategy,
                                     stats.attempts, stats.wins, stats.p99,
                                     stats.cap_hits,
                                     "round budget completed with zero wins")
            != 0) {
            snprintf(why, why_len, "dead-end append failed");
            free(stats.windows);
            return -1;
        }
        dead_end_written = 1;
        printf("[P1] DEAD_END strategy=%s attempts=%llu wins=0 p99=%llu "
               "appended=%s\n", strategy,
               (unsigned long long)stats.attempts,
               (unsigned long long)stats.p99, cfg->dead_ends_path);
    }
    printf("[P1] summary strategy=%s attempts=%llu wins=%llu esrch_main=%llu "
           "esrch_racer=%llu both_esrch=%llu timeout=%llu other=%llu "
           "cap_hits=%llu p50=%llu p90=%llu p99=%llu dropped_windows=%llu\n",
           strategy, (unsigned long long)stats.attempts,
           (unsigned long long)stats.wins,
           (unsigned long long)stats.esrch_main,
           (unsigned long long)stats.esrch_racer,
           (unsigned long long)stats.both_esrch,
           (unsigned long long)stats.timeouts,
           (unsigned long long)stats.other,
           (unsigned long long)stats.cap_hits,
           (unsigned long long)stats.p50,
           (unsigned long long)stats.p90,
           (unsigned long long)stats.p99,
           (unsigned long long)stats.dropped_windows);
    free(stats.windows);
    return 0;

fail:
    free(stats.windows);
    return -1;
}

int prolapse_p1_run(prolapse_config_t *cfg)
{
    p1_pool_t pool;
    FILE *out = stdout;
    const char *strategy = prolapse_config_get(cfg, "race.strategy");
    const char *strategies[3];
    int nstrategies = 0, si;
    uint64_t run_id;
    uint64_t seed = 0;
    uint32_t max_threads = PROLAPSE_DEFAULT_MAX_THREADS;
    uint32_t park_timeout = 2000000;
    uint64_t fanout = 1024, needed;
    uint32_t batch = 4;
    int cpu_a = 0, cpu_b = 1;
    int stop_all = 0;
    char why[512] = {0};
    int rc = 0;

    (void)prolapse_config_get_u64(cfg, "race.seed", &seed);
    (void)prolapse_config_get_u32(cfg, "race.max_threads", &max_threads);
    (void)prolapse_config_get_u32(cfg, "linger.park_timeout_us",
                                  &park_timeout);
    (void)prolapse_config_get_u64(cfg, "spray.fanout", &fanout);
    (void)prolapse_config_get_u32(cfg, "race.batch", &batch);
    g_round_counter = 0;
    g_attempt_seq = 0;
    g_call_seq = 0;
    memset(g_aio_buf, 0, sizeof(g_aio_buf));
    if (max_threads > P1_MAX_WORKERS)
        max_threads = P1_MAX_WORKERS;

    if (cfg->jsonl_path && strcmp(cfg->jsonl_path, "-") != 0) {
        out = fopen(cfg->jsonl_path, "w");
        if (!out) {
            fprintf(stderr, "prolapse p1: cannot write %s: %s\n",
                    cfg->jsonl_path, strerror(errno));
            return 2;
        }
    }

    if (prolapse_sys_bind(cfg) != 0) {
        if (out != stdout)
            fclose(out);
        return 2;
    }
    run_id = prolapse_now_us();
    if (run_id == 0)
        run_id = 1;
    run_id ^= (seed * 0x9E3779B97F4A7C15ull);
    printf("prolapse p1: run_id=%llu jsonl=%s seed=%llu\n",
           (unsigned long long)run_id,
           cfg->jsonl_path ? cfg->jsonl_path : "stdout",
           (unsigned long long)seed);

    if (prolapse_preflight_clock(why, sizeof(why)) != 0)
        goto preflight_fail;
    if (prolapse_preflight_cpuset(&cpu_a, &cpu_b, why, sizeof(why)) != 0)
        goto preflight_fail;
    needed = fanout + (uint64_t)max_threads * (uint64_t)batch * 2ull + 256ull;
    if (prolapse_preflight_rlimit(needed, why, sizeof(why)) != 0)
        goto preflight_fail;

    memset(&pool, 0, sizeof(pool));
    pool.max_threads = (int)max_threads;
    if (parking_preflight(&pool, cpu_a, cfg, why, sizeof(why)) != 0) {
        fprintf(stderr, "prolapse p1: PARKING PREFLIGHT FAILED: %s\n", why);
        (void)pool_drain(&pool, park_timeout, why, sizeof(why));
        rc = 2;
        goto out_close;
    }

    if (serialized_control(&pool, cfg, cpu_a, out, why, sizeof(why)) != 0) {
        fprintf(stderr, "prolapse p1: SERIALIZED CONTROL FAILED: %s\n", why);
        rc = 2;
        goto drain;
    }
    if (generation_control(&pool, cfg, cpu_a, out, why, sizeof(why)) != 0) {
        fprintf(stderr, "prolapse p1: GENERATION CONTROL FAILED: %s\n", why);
        rc = 2;
        goto drain;
    }

    if (!strategy)
        strategy = "all";
    if (strcmp(strategy, "all") == 0) {
        strategies[nstrategies++] = "same_barrier";
        strategies[nstrategies++] = "split_barrier";
        strategies[nstrategies++] = "split_stagger";
    } else {
        strategies[nstrategies++] = strategy;
    }

    for (si = 0; si < nstrategies && !stop_all; si++) {
        if (run_strategy(&pool, cfg, strategies[si], out, run_id, cpu_a, cpu_b,
                         &stop_all, why, sizeof(why)) != 0) {
            fprintf(stderr, "prolapse p1: STRATEGY FAILED strategy=%s: %s\n",
                    strategies[si], why);
            rc = 2;
            goto drain;
        }
    }

drain:
    if (pool_drain(&pool, park_timeout, why, sizeof(why)) != 0) {
        fprintf(stderr, "prolapse p1: DRAIN FAILED: %s\n", why);
        rc = 2;
    }
    if (rc == 0 && cfg->jsonl_path)
        printf("[P1] handoff=%s: run --stage p2 --input %s\n",
               cfg->jsonl_path, cfg->jsonl_path);
out_close:
    if (out != stdout)
        fclose(out);
    return rc;

preflight_fail:
    fprintf(stderr, "prolapse p1: PREFLIGHT FAILED: %s\n", why);
    if (out != stdout)
        fclose(out);
    return 2;
}
