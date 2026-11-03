#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define PROLAPSE_MAX_BATCH 32
#define PROLAPSE_MAX_CONFIG 128
#define PROLAPSE_MAX_THREADS 256
#define PROLAPSE_DEFAULT_MAX_THREADS 64

typedef uint32_t prolapse_aio_id_t;

typedef enum {
    PROLAPSE_OUTCOME_BOTH_ZERO = 0,
    PROLAPSE_OUTCOME_ESRCH_MAIN,
    PROLAPSE_OUTCOME_ESRCH_RACER,
    PROLAPSE_OUTCOME_BOTH_ESRCH,
    PROLAPSE_OUTCOME_TIMEOUT,
    PROLAPSE_OUTCOME_OTHER
} prolapse_outcome_t;

typedef struct {
    char key[64];
    char value[192];
    char value_1410[192];
    char phase[24];
    char source[24];
} prolapse_config_entry_t;

typedef struct {
    prolapse_config_entry_t entries[PROLAPSE_MAX_CONFIG];
    unsigned count;
    const char *stage;
    const char *jsonl_path;
    const char *input_path;
    const char *out_path;
    const char *dead_ends_path;
    const char *synthetic_path;
    const char *control;
    int synthetic_no_abort;
    int list_config;
} prolapse_config_t;

typedef struct {
    uint64_t run_id;
    uint64_t seq;
    uint64_t round;
    uint64_t round_id;
    const char *strategy;
    prolapse_aio_id_t id;
    uint32_t slot;
    uint32_t batch_size;
    uint64_t t_start_us;
    uint64_t t_end_us;
    uint64_t window_us;
    uint64_t sys624_main_before;
    uint64_t sys624_main_after;
    uint64_t sys624_racer_before;
    uint64_t sys624_racer_after;
    int main_ret;
    int main_err;
    int racer_ret;
    int racer_err;
    int main_seq;
    int racer_seq;
    int parked_live;
    prolapse_outcome_t outcome;
    int frozen;
    int candidate;
    int control;
    int synthetic;
    const char *note;
} prolapse_attempt_record_t;

/* config.c */
int prolapse_config_load(prolapse_config_t *cfg, int argc, char **argv);
const char *prolapse_config_get(const prolapse_config_t *cfg, const char *key);
const char *prolapse_config_source(const prolapse_config_t *cfg, const char *key);
int prolapse_config_get_u64(const prolapse_config_t *cfg, const char *key,
                            uint64_t *out);
int prolapse_config_get_u32(const prolapse_config_t *cfg, const char *key,
                            uint32_t *out);
int prolapse_config_get_int(const prolapse_config_t *cfg, const char *key,
                            int *out);
void prolapse_config_print(const prolapse_config_t *cfg, FILE *out);

/* classify.c */
int prolapse_is_esrch(int err);
int prolapse_is_zero(int err);
prolapse_outcome_t prolapse_classify(int main_ret, int racer_ret,
                                     int main_err, int racer_err);
prolapse_outcome_t prolapse_classify_windowed(int main_ret, int racer_ret,
                                              int main_err, int racer_err,
                                              uint64_t window_us,
                                              uint64_t threshold_us);
const char *prolapse_outcome_name(prolapse_outcome_t outcome);

/* jsonl.c */
void prolapse_json_escape(FILE *out, const char *s);
int prolapse_write_attempt(FILE *out, const prolapse_attempt_record_t *rec);
int prolapse_write_control_start(FILE *out, const char *name);
int prolapse_write_control(FILE *out, const char *name, uint64_t attempts,
                           uint64_t both_zero, uint64_t expected,
                           int pass, const char *detail);
int prolapse_append_dead_end(const char *path, const char *strategy,
                             uint64_t attempts, uint64_t wins,
                             uint64_t p99_window_us, uint64_t cap_hits,
                             const char *detail);

/* sys.c */
int prolapse_sys_bind(const prolapse_config_t *cfg);
uint64_t prolapse_now_us(void);
uint64_t prolapse_vm_counter(void);
int prolapse_pin_cpu(int cpu);
int prolapse_get_affinity(uint64_t *mask);
int prolapse_preflight_clock(char *why, size_t why_len);
int prolapse_preflight_cpuset(int *cpu_a, int *cpu_b, char *why,
                              size_t why_len);
int prolapse_preflight_rlimit(uint64_t needed, char *why, size_t why_len);
void prolapse_sleep_us(uint64_t us);
long prolapse_aio_submit_read(const int *fds, uint32_t n, void *buf,
                              prolapse_aio_id_t *ids, int *errs);
long prolapse_aio_cancel(const prolapse_aio_id_t *ids, uint32_t n,
                         uint32_t *states);
long prolapse_aio_poll(const prolapse_aio_id_t *ids, uint32_t n,
                       uint32_t *states);
long prolapse_aio_delete(const prolapse_aio_id_t *ids, uint32_t n, int *errs);

/* transport.c */
typedef struct {
    int target;
    int peer;
    int linger_onoff;
    int linger_seconds;
} prolapse_transport_t;
int prolapse_transport_open(prolapse_transport_t *t, const char *kind,
                            int linger_onoff, int linger_seconds,
                            char *why, size_t why_len);
void prolapse_transport_close(prolapse_transport_t *t);
int prolapse_transport_release(prolapse_transport_t *t, char *why,
                               size_t why_len);
int prolapse_transport_release_peer(int *peer_fd, char *why, size_t why_len);

/* p1_race.c / p2_win.c / synthetic.c */
int prolapse_p1_run(prolapse_config_t *cfg);
int prolapse_p2_run(prolapse_config_t *cfg);
int prolapse_synthetic_run(prolapse_config_t *cfg);
