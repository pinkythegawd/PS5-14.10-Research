#include "prolapse.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_classify(void)
{
    assert(prolapse_is_esrch(3));
    assert(prolapse_is_esrch((int)0x80020003u));
    assert(!prolapse_is_esrch(0));
    assert(!prolapse_is_esrch(22));

    assert(prolapse_classify(0, 0, 0, 0) == PROLAPSE_OUTCOME_BOTH_ZERO);
    assert(prolapse_classify(0, 0, 0, 3) == PROLAPSE_OUTCOME_ESRCH_RACER);
    assert(prolapse_classify(0, 0, (int)0x80020003u, 0) ==
           PROLAPSE_OUTCOME_ESRCH_MAIN);
    assert(prolapse_classify(0, 0, 3, (int)0x80020003u) ==
           PROLAPSE_OUTCOME_BOTH_ESRCH);
    assert(prolapse_classify(0, 0, 22, 0) == PROLAPSE_OUTCOME_OTHER);
}

static void test_jsonl(void)
{
    FILE *f = tmpfile();
    char buf[4096];
    size_t n;
    prolapse_attempt_record_t rec;

    assert(f != NULL);
    memset(&rec, 0, sizeof(rec));
    rec.run_id = 7;
    rec.seq = 1;
    rec.round = 2;
    rec.round_id = 99;
    rec.strategy = "split_barrier";
    rec.id = 0x4205;
    rec.slot = 1;
    rec.batch_size = 4;
    rec.t_start_us = 100;
    rec.t_end_us = 180;
    rec.window_us = 80;
    rec.main_ret = 0;
    rec.main_err = 0;
    rec.racer_ret = 0;
    rec.racer_err = (int)0x80020003u;
    rec.main_seq = 1;
    rec.racer_seq = 2;
    rec.parked_live = 0;
    rec.outcome = PROLAPSE_OUTCOME_ESRCH_RACER;
    rec.frozen = 0;
    rec.candidate = 0;
    assert(prolapse_write_attempt(f, &rec) == 0);
    rewind(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    assert(strstr(buf, "\"record\":\"attempt\"") != NULL);
    assert(strstr(buf, "\"id\":16901") != NULL);
    assert(strstr(buf, "\"window_us\":80") != NULL);
    assert(strstr(buf, "\"outcome\":\"esrch_racer\"") != NULL);
    assert(strstr(buf, "\"sys624_kind\":\"vm_operation_counter_ordering_token\"")
           != NULL);
    assert(strstr(buf, "\"synthetic\":false") != NULL);
    assert(strstr(buf, "\"sys624_unverified_on_console\":true") != NULL);
    assert(strstr(buf, "\"clock_src\":\"libc\"") != NULL);
}

static void test_dead_end(void)
{
    char path[128];
    FILE *f;
    char buf[1024];
    size_t n;
    snprintf(path, sizeof(path), "/tmp/prolapse_dead_%ld.md", (long)getpid());
    unlink(path);
    assert(prolapse_append_dead_end(path, "split_stagger", 1000, 0, 42, 2,
                                    "unit test") == 0);
    f = fopen(path, "r");
    assert(f != NULL);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    unlink(path);
    assert(strstr(buf, "split_stagger") != NULL);
    assert(strstr(buf, "attempts: 1000") != NULL);
    assert(strstr(buf, "p99_window_us: 42") != NULL);
    assert(strstr(buf, "cap_hits: 2") != NULL);
}

static int quiet_stderr_begin(void)
{
    int saved = dup(STDERR_FILENO);
    int nullfd = open("/dev/null", O_WRONLY);
    assert(saved >= 0 && nullfd >= 0);
    assert(dup2(nullfd, STDERR_FILENO) == STDERR_FILENO);
    close(nullfd);
    return saved;
}

static void quiet_stderr_end(int saved)
{
    assert(dup2(saved, STDERR_FILENO) == STDERR_FILENO);
    close(saved);
}

static void test_config(void)
{
    prolapse_config_t cfg;
    char *argv[] = {
        "prolapse", "--stage", "p1", "--set", "race.batch=2",
        "--strategy", "split_stagger", "--max-threads", "2",
        "--no-stop-on-first-win", "--jsonl", "/tmp/x.jsonl", NULL
    };
    uint32_t u = 0;
    int i = 0;
    assert(prolapse_config_load(&cfg, 12, argv) == 0);
    assert(strcmp(prolapse_config_get(&cfg, "race.strategy"),
                  "split_stagger") == 0);
    assert(prolapse_config_get_u32(&cfg, "race.batch", &u) == 0 && u == 2);
    assert(prolapse_config_get_u32(&cfg, "race.max_threads", &u) == 0 && u == 2);
    assert(prolapse_config_get_int(&cfg, "race.stop_on_first_win", &i) == 0 &&
           i == 0);
    assert(prolapse_config_get_u64(&cfg, "sys.aio_multi_delete", &(uint64_t){0})
           == 0);

    {
        int saved = quiet_stderr_begin();
        char *bad[] = {"prolapse", "--stage", "p1", "--no-stop-on-first-win",
                       NULL};
        assert(prolapse_config_load(&cfg, 4, bad) != 0);
        quiet_stderr_end(saved);
    }
    {
        int saved = quiet_stderr_begin();
        char *bad[] = {"prolapse", "--stage", "p1", "--set",
                       "target.profile=14.10", NULL};
        assert(prolapse_config_load(&cfg, 5, bad) != 0);
        quiet_stderr_end(saved);
    }
}

int main(void)
{
    test_classify();
    test_jsonl();
    test_dead_end();
    test_config();
    printf("test_core: ok\n");
    return 0;
}
