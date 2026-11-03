#include "prolapse.h"
#include "prolapse_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0)
        return;
    if (!src)
        src = "";
    snprintf(dst, cap, "%s", src);
}

static prolapse_config_entry_t *find_entry(prolapse_config_t *cfg,
                                           const char *key)
{
    unsigned i;
    for (i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0)
            return &cfg->entries[i];
    }
    return NULL;
}

static const prolapse_config_entry_t *find_entry_const(
    const prolapse_config_t *cfg, const char *key)
{
    unsigned i;
    for (i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].key, key) == 0)
            return &cfg->entries[i];
    }
    return NULL;
}

static int set_entry(prolapse_config_t *cfg, const char *key,
                     const char *value, const char *source)
{
    prolapse_config_entry_t *e = find_entry(cfg, key);
    if (!e) {
        if (cfg->count >= PROLAPSE_MAX_CONFIG)
            return -1;
        e = &cfg->entries[cfg->count++];
        memset(e, 0, sizeof(*e));
        copy_str(e->key, sizeof(e->key), key);
        copy_str(e->value_1410, sizeof(e->value_1410), value);
        copy_str(e->phase, sizeof(e->phase), "P1");
    }
    copy_str(e->value, sizeof(e->value), value);
    copy_str(e->source, sizeof(e->source), source);
    return 0;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtoull(s, &end, 0);
    if (errno || !end || *end != '\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int get_u64_raw(const prolapse_config_t *cfg, const char *key,
                       uint64_t *out, const char **source)
{
    const prolapse_config_entry_t *e = find_entry_const(cfg, key);
    if (!e)
        return -1;
    if (parse_u64(e->value, out) == 0) {
        if (source)
            *source = e->source;
        return 0;
    }
    /* The 14.00 column deliberately says "verify on target" for syscall
     * numbers.  Use the ledger's 14.10 value only as an explicitly-labelled
     * fallback; it is never silently marked confirmed. */
    if (strncmp(key, "sys.", 4) == 0 && parse_u64(e->value_1410, out) == 0) {
        if (source)
            *source = "ledger_14.10_assumed";
        return 0;
    }
    return -1;
}

const char *prolapse_config_get(const prolapse_config_t *cfg, const char *key)
{
    const prolapse_config_entry_t *e = find_entry_const(cfg, key);
    if (!e)
        return NULL;
    return e->value;
}

const char *prolapse_config_source(const prolapse_config_t *cfg,
                                   const char *key)
{
    const prolapse_config_entry_t *e = find_entry_const(cfg, key);
    return e ? e->source : NULL;
}

int prolapse_config_get_u64(const prolapse_config_t *cfg, const char *key,
                            uint64_t *out)
{
    return get_u64_raw(cfg, key, out, NULL);
}

int prolapse_config_get_u32(const prolapse_config_t *cfg, const char *key,
                            uint32_t *out)
{
    uint64_t v;
    if (get_u64_raw(cfg, key, &v, NULL) != 0 || v > 0xffffffffu)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

int prolapse_config_get_int(const prolapse_config_t *cfg, const char *key,
                            int *out)
{
    uint64_t v;
    if (get_u64_raw(cfg, key, &v, NULL) != 0 || v > 0x7fffffffu)
        return -1;
    *out = (int)v;
    return 0;
}

static int check_range(const prolapse_config_t *cfg, const char *key,
                       uint64_t lo, uint64_t hi)
{
    uint64_t v;
    if (get_u64_raw(cfg, key, &v, NULL) != 0) {
        fprintf(stderr, "prolapse: %s is not numeric\n", key);
        return -1;
    }
    if (v < lo || v > hi) {
        fprintf(stderr, "prolapse: %s=%llu outside [%llu,%llu]\n", key,
                (unsigned long long)v, (unsigned long long)lo,
                (unsigned long long)hi);
        return -1;
    }
    return 0;
}

static int validate(prolapse_config_t *cfg)
{
    const char *strategy = prolapse_config_get(cfg, "race.strategy");
    const char *profile = prolapse_config_get(cfg, "target.profile");
    uint64_t stop;

    if (!profile || strcmp(profile, "14.00_v59") != 0) {
        fprintf(stderr,
                "prolapse: this round only supports target.profile=14.00_v59 "
                "(got %s); 14.10 offsets are not ported\n",
                profile ? profile : "(unset)");
        return -1;
    }
    if (!strategy ||
        (strcmp(strategy, "all") != 0 &&
         strcmp(strategy, "split_barrier") != 0 &&
         strcmp(strategy, "same_barrier") != 0 &&
         strcmp(strategy, "split_stagger") != 0)) {
        fprintf(stderr, "prolapse: bad race.strategy '%s'\n",
                strategy ? strategy : "(unset)");
        return -1;
    }
    if (check_range(cfg, "race.batch", 1, PROLAPSE_MAX_BATCH) ||
        check_range(cfg, "race.rounds", 1, 5000000) ||
        check_range(cfg, "race.stagger_us", 0, 500) ||
        check_range(cfg, "race.max_threads", 2, PROLAPSE_MAX_THREADS) ||
        check_range(cfg, "race.control_rounds", 8, 100000) ||
        check_range(cfg, "race.loss_threshold", 1, 5000000) ||
        check_range(cfg, "linger.onoff", 0, 1) ||
        check_range(cfg, "linger.seconds", 0, 60) ||
        check_range(cfg, "race.terminal_timeout_us", 1000, 10000000) ||
        check_range(cfg, "linger.park_timeout_us", 1000, 60000000) ||
        get_u64_raw(cfg, "race.stop_on_first_win", &stop, NULL) != 0 ||
        stop > 1) {
        return -1;
    }
    if (stop == 0 && !cfg->jsonl_path) {
        fprintf(stderr,
                "prolapse: --no-stop-on-first-win needs --jsonl so the "
                "denominator is durable\n");
        return -1;
    }
    return 0;
}

static int apply_set(prolapse_config_t *cfg, const char *arg)
{
    char key[64];
    const char *eq = strchr(arg, '=');
    size_t n;
    if (!eq) {
        fprintf(stderr, "prolapse: --set expects key=value\n");
        return -1;
    }
    n = (size_t)(eq - arg);
    if (n == 0 || n >= sizeof(key)) {
        fprintf(stderr, "prolapse: --set key too long\n");
        return -1;
    }
    memcpy(key, arg, n);
    key[n] = '\0';
    if (!find_entry(cfg, key)) {
        fprintf(stderr, "prolapse: unknown --set key '%s'\n", key);
        return -1;
    }
    if (strncmp(key, "p4.", 3) == 0 ||
        strcmp(key, "aio.entry_notify_off") == 0 ||
        strcmp(key, "aio.state_size") == 0) {
        fprintf(stderr, "prolapse: --set %s is P4-only and forbidden here\n",
                key);
        return -1;
    }
    return set_entry(cfg, key, eq + 1, "override");
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --stage p1|p2 [options]\n"
        "  P1: --strategy all|split_barrier|same_barrier|split_stagger\n"
        "      --rounds N --stop-on-first-win|--no-stop-on-first-win\n"
        "      --max-threads N --set key=value --jsonl PATH --seed N\n"
        "      [--dead-ends PATH] [--loss-threshold N] [--control-rounds N]\n"
        "  P2: --stage p2 --input PATH [--jsonl PATH]\n"
        "  common: --list-config --help\n",
        argv0);
}

int prolapse_config_load(prolapse_config_t *cfg, int argc, char **argv)
{
    unsigned i;
    int j;

    memset(cfg, 0, sizeof(*cfg));
    for (i = 0; i < g_prolapse_ledger_count; i++) {
        prolapse_config_entry_t *e = &cfg->entries[cfg->count++];
        memset(e, 0, sizeof(*e));
        copy_str(e->key, sizeof(e->key), g_prolapse_ledger[i].key);
        copy_str(e->value, sizeof(e->value), g_prolapse_ledger[i].value_1400);
        copy_str(e->value_1410, sizeof(e->value_1410),
                 g_prolapse_ledger[i].value_1410);
        copy_str(e->phase, sizeof(e->phase), g_prolapse_ledger[i].phase);
        copy_str(e->source, sizeof(e->source), "ledger");
    }

    cfg->stage = NULL;
    cfg->dead_ends_path = "docs/DEAD_ENDS.md";

    for (j = 1; j < argc; j++) {
        const char *a = argv[j];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            cfg->stage = "help";
            return 0;
        } else if (strcmp(a, "--stage") == 0 && j + 1 < argc) {
            cfg->stage = argv[++j];
        } else if (strcmp(a, "--strategy") == 0 && j + 1 < argc) {
            if (set_entry(cfg, "race.strategy", argv[++j], "cli") != 0)
                return -1;
        } else if (strcmp(a, "--rounds") == 0 && j + 1 < argc) {
            if (set_entry(cfg, "race.rounds", argv[++j], "cli") != 0)
                return -1;
        } else if (strcmp(a, "--max-threads") == 0 && j + 1 < argc) {
            if (set_entry(cfg, "race.max_threads", argv[++j], "cli") != 0)
                return -1;
        } else if (strcmp(a, "--seed") == 0 && j + 1 < argc) {
            if (set_entry(cfg, "race.seed", argv[++j], "cli") != 0)
                return -1;
        } else if (strcmp(a, "--loss-threshold") == 0 && j + 1 < argc) {
            if (set_entry(cfg, "race.loss_threshold", argv[++j], "cli") != 0)
                return -1;
        } else if (strcmp(a, "--control-rounds") == 0 && j + 1 < argc) {
            if (set_entry(cfg, "race.control_rounds", argv[++j], "cli") != 0)
                return -1;
        } else if (strcmp(a, "--stop-on-first-win") == 0) {
            if (set_entry(cfg, "race.stop_on_first_win", "1", "cli") != 0)
                return -1;
        } else if (strcmp(a, "--no-stop-on-first-win") == 0) {
            if (set_entry(cfg, "race.stop_on_first_win", "0", "cli") != 0)
                return -1;
        } else if (strcmp(a, "--jsonl") == 0 && j + 1 < argc) {
            cfg->jsonl_path = argv[++j];
        } else if (strcmp(a, "--input") == 0 && j + 1 < argc) {
            cfg->input_path = argv[++j];
        } else if (strcmp(a, "--out") == 0 && j + 1 < argc) {
            cfg->out_path = argv[++j];
        } else if (strcmp(a, "--dead-ends") == 0 && j + 1 < argc) {
            cfg->dead_ends_path = argv[++j];
        } else if (strcmp(a, "--set") == 0 && j + 1 < argc) {
            if (apply_set(cfg, argv[++j]) != 0)
                return -1;
        } else if (strcmp(a, "--list-config") == 0) {
            cfg->list_config = 1;
        } else {
            fprintf(stderr, "prolapse: unknown or incomplete option '%s'\n", a);
            usage(argv[0]);
            return -1;
        }
    }

    if (!cfg->stage) {
        fprintf(stderr, "prolapse: --stage p1|p2 is required\n");
        usage(argv[0]);
        return -1;
    }
    if (strcmp(cfg->stage, "help") == 0)
        return 0;
    if (strcmp(cfg->stage, "p1") != 0 && strcmp(cfg->stage, "p2") != 0) {
        fprintf(stderr, "prolapse: unknown stage '%s'\n", cfg->stage);
        return -1;
    }
    if (strcmp(cfg->stage, "p2") == 0 && !cfg->input_path) {
        fprintf(stderr, "prolapse: --stage p2 requires --input PATH\n");
        return -1;
    }
    return validate(cfg);
}

void prolapse_config_print(const prolapse_config_t *cfg, FILE *out)
{
    unsigned i;
    for (i = 0; i < cfg->count; i++) {
        const prolapse_config_entry_t *e = &cfg->entries[i];
        const char *allowed = "yes";
        const char *value = e->value;
        const char *source = e->source;
        uint64_t ignored;
        if (strncmp(e->key, "sys.", 4) == 0 &&
            parse_u64(e->value, &ignored) != 0 &&
            parse_u64(e->value_1410, &ignored) == 0) {
            value = e->value_1410;
            if (strcmp(source, "ledger") == 0)
                source = "ledger_14.10_assumed";
        }
        if (strncmp(e->key, "p4.", 3) == 0 ||
            strcmp(e->key, "aio.entry_notify_off") == 0 ||
            strcmp(e->key, "aio.state_size") == 0)
            allowed = "no (P4)";
        else if (strcmp(e->phase, "P3") == 0)
            allowed = "read-only (P3 blocked)";
        fprintf(out, "%-24s = %-28s source=%-18s phase=%-7s allowed=%s\n",
                e->key, value, source, e->phase, allowed);
    }
}
