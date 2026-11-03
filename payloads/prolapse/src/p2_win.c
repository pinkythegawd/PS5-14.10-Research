#include "prolapse.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P2_LINE_MAX 16384
#define P2_MAX_LINES PROLAPSE_MAX_BATCH

typedef struct {
    uint64_t round_id;
    char strategy[64];
    unsigned batch_size;
    unsigned count;
    struct {
        int slot;
        prolapse_aio_id_t id;
        int main_ret;
        int main_err;
        int racer_ret;
        int racer_err;
        int main_seq;
        int racer_seq;
        int frozen;
        int candidate;
        int present;
    } lines[P2_MAX_LINES];
} p2_round_t;

static const char *find_key(const char *line, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(line, pat);
    if (!p)
        return NULL;
    return p + strlen(pat);
}

static int parse_u64_field(const char *line, const char *key, uint64_t *out)
{
    const char *p = find_key(line, key);
    char *end = NULL;
    unsigned long long v;
    if (!p)
        return -1;
    errno = 0;
    v = strtoull(p, &end, 10);
    if (errno || !end || (*end != ',' && *end != '}'))
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static int parse_i32_field(const char *line, const char *key, int *out)
{
    const char *p = find_key(line, key);
    char *end = NULL;
    long v;
    if (!p)
        return -1;
    errno = 0;
    v = strtol(p, &end, 10);
    if (errno || !end || (*end != ',' && *end != '}'))
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_bool_field(const char *line, const char *key, int *out)
{
    const char *p = find_key(line, key);
    if (!p)
        return -1;
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int parse_str_field(const char *line, const char *key, char *out,
                           size_t out_len)
{
    const char *p = find_key(line, key);
    size_t n = 0;
    if (!p || *p != '"')
        return -1;
    p++;
    while (*p && *p != '"' && n + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[n++] = '\n'; break;
            case 'r': out[n++] = '\r'; break;
            case 't': out[n++] = '\t'; break;
            default:  out[n++] = *p; break;
            }
            p++;
        } else {
            out[n++] = *p++;
        }
    }
    if (*p != '"')
        return -1;
    out[n] = '\0';
    return 0;
}

static int parse_line(const char *line, p2_round_t *round, int *is_control,
                      int *is_attempt)
{
    uint64_t u;
    char tmp[64];
    unsigned idx;

    *is_control = 0;
    *is_attempt = 0;
    if (strstr(line, "\"record\":\"control\"")) {
        *is_control = 1;
        return 0;
    }
    if (!strstr(line, "\"record\":\"attempt\""))
        return 0;
    *is_attempt = 1;

    if (parse_u64_field(line, "round_id", &u) != 0)
        return -1;
    round->round_id = u;
    memset(round->strategy, 0, sizeof(round->strategy));
    if (parse_str_field(line, "strategy", tmp, sizeof(tmp)) == 0)
        snprintf(round->strategy, sizeof(round->strategy), "%s", tmp);
    if (parse_u64_field(line, "batch_size", &u) == 0)
        round->batch_size = (unsigned)u;

    if (round->count >= P2_MAX_LINES)
        return -1;
    idx = round->count++;
    memset(&round->lines[idx], 0, sizeof(round->lines[idx]));
    round->lines[idx].present = 1;

    if (parse_u64_field(line, "id", &u) == 0)
        round->lines[idx].id = (prolapse_aio_id_t)u;
    else
        return -1;
    if (parse_u64_field(line, "slot", &u) == 0)
        round->lines[idx].slot = (int)u;
    else
        round->lines[idx].slot = (int)idx;

    if (parse_i32_field(line, "main_ret", &round->lines[idx].main_ret) != 0 ||
        parse_i32_field(line, "main_err", &round->lines[idx].main_err) != 0 ||
        parse_i32_field(line, "racer_ret", &round->lines[idx].racer_ret) != 0 ||
        parse_i32_field(line, "racer_err", &round->lines[idx].racer_err) != 0 ||
        parse_i32_field(line, "main_seq", &round->lines[idx].main_seq) != 0 ||
        parse_i32_field(line, "racer_seq", &round->lines[idx].racer_seq) != 0)
        return -1;
    if (parse_bool_field(line, "frozen", &round->lines[idx].frozen) != 0)
        round->lines[idx].frozen = 0;
    if (parse_bool_field(line, "candidate", &round->lines[idx].candidate) != 0)
        round->lines[idx].candidate = 0;
    return 0;
}

static int line_is_both_zero(const p2_round_t *r, unsigned i)
{
    return prolapse_classify(r->lines[i].main_ret, r->lines[i].racer_ret,
                             r->lines[i].main_err, r->lines[i].racer_err) ==
           PROLAPSE_OUTCOME_BOTH_ZERO;
}

static int has_frozen_candidate(const p2_round_t *r)
{
    unsigned i;
    for (i = 0; i < r->count; i++) {
        if (r->lines[i].frozen && line_is_both_zero(r, i))
            return 1;
    }
    return 0;
}

static int flush_round(FILE *out, const p2_round_t *r, uint64_t *candidates,
                       uint64_t *suspects)
{
    unsigned i;
    int round_has_candidate = 0;

    if (r->count == 0)
        return 0;
    if (!has_frozen_candidate(r))
        return 0;

    round_has_candidate = 1;
    fprintf(out,
            "{\"record\":\"p2_round\",\"round_id\":%llu,\"strategy\":\"",
            (unsigned long long)r->round_id);
    prolapse_json_escape(out, r->strategy);
    fprintf(out, "\",\"batch_size\":%u,\"lines\":%u,\"frozen\":true}\n",
            r->batch_size ? r->batch_size : r->count, r->count);

    for (i = 0; i < r->count; i++) {
        const int both_zero = line_is_both_zero(r, i);
        int winner = 0;
        int second_is_racer = 1;
        int suspect = 0;
        int suspect_err = 0;

        if (both_zero) {
            /* The second zero is the later caller.  Sequences are per-call
             * ordering tokens, not timestamps. */
            second_is_racer = (r->lines[i].racer_seq >= r->lines[i].main_seq);
            winner = 1;
            (*candidates)++;
        }
        if (r->lines[i].main_err != 0 &&
            !prolapse_is_esrch(r->lines[i].main_err)) {
            suspect = 1;
            suspect_err = r->lines[i].main_err;
        }
        if (r->lines[i].racer_err != 0 &&
            !prolapse_is_esrch(r->lines[i].racer_err)) {
            suspect = 1;
            if (!suspect_err)
                suspect_err = r->lines[i].racer_err;
        }
        if (r->lines[i].main_ret != 0 && r->lines[i].main_err == 0) {
            suspect = 1;
            if (!suspect_err)
                suspect_err = r->lines[i].main_ret;
        }
        if (r->lines[i].racer_ret != 0 && r->lines[i].racer_err == 0) {
            suspect = 1;
            if (!suspect_err)
                suspect_err = r->lines[i].racer_ret;
        }
        if (suspect)
            (*suspects)++;

        fprintf(out,
                "{\"record\":\"p2_id\",\"round_id\":%llu,\"strategy\":\"",
                (unsigned long long)r->round_id);
        prolapse_json_escape(out, r->strategy);
        fprintf(out,
                "\",\"slot\":%d,\"id\":%u,\"id_hex\":\"0x%08x\","
                "\"ret\":%d,\"err\":%d,\"seq\":%d,"
                "\"main_ret\":%d,\"main_err\":%d,\"main_err_hex\":\"0x%08x\","
                "\"racer_ret\":%d,\"racer_err\":%d,"
                "\"racer_err_hex\":\"0x%08x\","
                "\"main_seq\":%d,\"racer_seq\":%d,"
                "\"both_zero\":%s,\"winner_id\":%u,\"winner_slot\":%d,"
                "\"winner_role\":\"%s\",\"false_winner_suspect\":%s,"
                "\"suspect_err\":%d}\n",
                r->lines[i].slot, (unsigned)r->lines[i].id,
                (unsigned)r->lines[i].id,
                r->lines[i].main_ret, r->lines[i].main_err,
                r->lines[i].main_seq,
                r->lines[i].main_ret, r->lines[i].main_err,
                (unsigned)r->lines[i].main_err,
                r->lines[i].racer_ret, r->lines[i].racer_err,
                (unsigned)r->lines[i].racer_err,
                r->lines[i].main_seq, r->lines[i].racer_seq,
                both_zero ? "true" : "false",
                both_zero ? (unsigned)r->lines[i].id : 0,
                both_zero ? r->lines[i].slot : -1,
                both_zero ? (second_is_racer ? "racer" : "main") : "none",
                suspect ? "true" : "false", suspect_err);
        if (winner) {
            fprintf(out,
                    "{\"record\":\"p2_winner\",\"round_id\":%llu,"
                    "\"strategy\":\"",
                    (unsigned long long)r->round_id);
            prolapse_json_escape(out, r->strategy);
            fprintf(out,
                    "\",\"slot\":%d,\"winner_id\":%u,"
                    "\"winner_role\":\"%s\"}\n",
                    r->lines[i].slot, (unsigned)r->lines[i].id,
                    second_is_racer ? "racer" : "main");
        }
    }
    (void)round_has_candidate;
    return 0;
}

int prolapse_p2_run(prolapse_config_t *cfg)
{
    FILE *in = NULL;
    FILE *out = stdout;
    char line[P2_LINE_MAX];
    p2_round_t round;
    uint64_t rounds = 0, candidates = 0, suspects = 0;
    uint64_t current_round = 0;
    int have_round = 0;
    int rc = 0;
    const char *out_path = cfg->out_path ? cfg->out_path : cfg->jsonl_path;

    if (!cfg->input_path) {
        fprintf(stderr, "prolapse p2: --input PATH is required\n");
        return 2;
    }
    if (strcmp(cfg->input_path, "-") == 0) {
        in = stdin;
    } else {
        in = fopen(cfg->input_path, "r");
        if (!in) {
            fprintf(stderr, "prolapse p2: cannot open %s: %s\n",
                    cfg->input_path, strerror(errno));
            return 2;
        }
    }
    if (out_path && *out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "prolapse p2: cannot write %s: %s\n",
                    out_path, strerror(errno));
            if (in != stdin)
                fclose(in);
            return 2;
        }
    }

    memset(&round, 0, sizeof(round));
    while (fgets(line, sizeof(line), in)) {
        p2_round_t parsed;
        int is_control = 0, is_attempt = 0;
        size_t n = strlen(line);
        if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
            fprintf(stderr, "prolapse p2: input line exceeds %d bytes\n",
                    P2_LINE_MAX);
            rc = 2;
            break;
        }
        memset(&parsed, 0, sizeof(parsed));
        if (parse_line(line, &parsed, &is_control, &is_attempt) != 0) {
            fprintf(stderr, "prolapse p2: malformed attempt line: %s", line);
            rc = 2;
            break;
        }
        if (is_control || !is_attempt)
            continue;
        if (!have_round || parsed.round_id != current_round) {
            if (have_round)
                flush_round(out, &round, &candidates, &suspects);
            round = parsed;
            current_round = parsed.round_id;
            have_round = 1;
            rounds++;
        } else {
            unsigned i;
            for (i = 0; i < parsed.count && round.count < P2_MAX_LINES; i++)
                round.lines[round.count++] = parsed.lines[i];
        }
    }
    if (have_round && rc == 0)
        flush_round(out, &round, &candidates, &suspects);

    fprintf(out,
            "{\"record\":\"p2_summary\",\"input\":\"%s\","
            "\"rounds_seen\":%llu,\"frozen_candidates\":%llu,"
            "\"false_winner_suspects\":%llu}\n",
            cfg->input_path, (unsigned long long)rounds,
            (unsigned long long)candidates,
            (unsigned long long)suspects);

    if (in != stdin)
        fclose(in);
    if (out != stdout)
        fclose(out);
    printf("prolapse p2: frozen_candidates=%llu false_winner_suspects=%llu\n",
           (unsigned long long)candidates, (unsigned long long)suspects);
    return rc;
}
