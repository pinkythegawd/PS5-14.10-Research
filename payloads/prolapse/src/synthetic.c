#include "prolapse.h"

#ifdef PROLAPSE_HOST

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYNTHETIC_LINE_MAX 4096

static int parse_int_field(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtol(s, &end, 0);
    if (errno || !end || *end != '\0' ||
        v < -2147483648L || v > 0xffffffffL)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_u64_field(const char *s, uint64_t *out)
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

static int parse_outcome_token(const char *tok, prolapse_outcome_t *out)
{
    int i;
    if (!tok || !*tok)
        return -1;
    for (i = 0; i <= PROLAPSE_OUTCOME_OTHER; i++) {
        if (strcmp(prolapse_outcome_name((prolapse_outcome_t)i), tok) == 0) {
            *out = (prolapse_outcome_t)i;
            return 0;
        }
    }
    return -1;
}

/*
 * CSV row: seq,main_ret,main_err,racer_ret,racer_err[,outcome][,note]
 * The outcome column, when non-empty, overrides prolapse_classify (used for
 * timeout rows).  The note column is free text: everything after its comma is
 * kept verbatim, commas included.
 * Returns 1 for blank/comment/header lines, 0 for a data row, -1 on error.
 */
static int parse_row(char *line, uint64_t row_idx, uint64_t *seq, int *mr,
                     int *me, int *rr, int *re, prolapse_outcome_t *override,
                     int *has_override, char *note, size_t note_cap,
                     char *why, size_t why_len)
{
    char *fields[7];
    int nf = 0;
    char *p = line;
    size_t nl = strlen(line);

    while (nl > 0 && (line[nl - 1] == '\n' || line[nl - 1] == '\r'))
        line[--nl] = '\0';
    if (line[0] == '\0' || line[0] == '#')
        return 1;
    if (row_idx == 1 && strncmp(line, "seq,", 4) == 0)
        return 1;

    fields[nf++] = p;
    for (; *p && nf < 7; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[nf++] = p + 1;
        }
    }
    if (nf < 5) {
        snprintf(why, why_len, "row %llu: need at least 5 fields, got %d",
                 (unsigned long long)row_idx, nf);
        return -1;
    }
    if (parse_u64_field(fields[0], seq) != 0 ||
        parse_int_field(fields[1], mr) != 0 ||
        parse_int_field(fields[2], me) != 0 ||
        parse_int_field(fields[3], rr) != 0 ||
        parse_int_field(fields[4], re) != 0) {
        snprintf(why, why_len, "row %llu: bad numeric field",
                 (unsigned long long)row_idx);
        return -1;
    }
    *has_override = 0;
    if (nf >= 6 && fields[5][0] != '\0') {
        if (parse_outcome_token(fields[5], override) != 0) {
            snprintf(why, why_len, "row %llu: unknown outcome '%s'",
                     (unsigned long long)row_idx, fields[5]);
            return -1;
        }
        *has_override = 1;
    }
    note[0] = '\0';
    if (nf >= 7) {
        const char *s = fields[6];
        while (*s == ' ' || *s == '\t')
            s++;
        snprintf(note, note_cap, "%s", s);
    }
    return 0;
}

int prolapse_synthetic_run(prolapse_config_t *cfg)
{
    FILE *in;
    FILE *out = stdout;
    char line[SYNTHETIC_LINE_MAX];
    char note[256];
    char why[256];
    uint64_t hist[PROLAPSE_OUTCOME_OTHER + 1] = {0};
    uint64_t rows = 0, both_zero = 0, row_idx = 0;
    uint64_t seed = 0, run_id;
    int rc = 0;

    (void)prolapse_config_get_u64(cfg, "race.seed", &seed);
    run_id = 0x5A17E7ull ^ (seed * 0x9E3779B97F4A7C15ull);
    if (run_id == 0)
        run_id = 1;

    if (strcmp(cfg->synthetic_path, "-") == 0) {
        in = stdin;
    } else {
        in = fopen(cfg->synthetic_path, "r");
        if (!in) {
            fprintf(stderr, "prolapse synthetic: cannot open %s: %s\n",
                    cfg->synthetic_path, strerror(errno));
            return 2;
        }
    }
    if (cfg->jsonl_path && strcmp(cfg->jsonl_path, "-") != 0) {
        out = fopen(cfg->jsonl_path, "w");
        if (!out) {
            fprintf(stderr, "prolapse synthetic: cannot write %s: %s\n",
                    cfg->jsonl_path, strerror(errno));
            if (in != stdin)
                fclose(in);
            return 2;
        }
    }

    while (fgets(line, sizeof(line), in)) {
        uint64_t seq;
        int mr, me, rr, re, has_override;
        prolapse_outcome_t override, outcome;
        prolapse_attempt_record_t rec;
        int pr;

        if (!strchr(line, '\n') && !feof(in)) {
            fprintf(stderr, "prolapse synthetic: line exceeds %d bytes\n",
                    SYNTHETIC_LINE_MAX);
            rc = 2;
            break;
        }
        pr = parse_row(line, ++row_idx, &seq, &mr, &me, &rr, &re,
                       &override, &has_override, note, sizeof(note),
                       why, sizeof(why));
        if (pr > 0)
            continue;
        if (pr < 0) {
            fprintf(stderr, "prolapse synthetic: %s\n", why);
            rc = 2;
            break;
        }

        outcome = has_override ? override
                 : prolapse_classify(mr, rr, me, re);
        memset(&rec, 0, sizeof(rec));
        rec.run_id = run_id;
        rec.seq = seq;
        rec.round = row_idx;
        rec.round_id = 0;
        rec.strategy = "synthetic";
        rec.id = (prolapse_aio_id_t)(0x5A0000u +
                                     (unsigned)(row_idx & 0xFFFFu));
        rec.slot = 0;
        rec.batch_size = 1;
        rec.main_ret = mr;
        rec.main_err = me;
        rec.racer_ret = rr;
        rec.racer_err = re;
        rec.outcome = outcome;
        rec.synthetic = 1;
        rec.note = note[0] ? note : NULL;
        if (prolapse_write_attempt(out, &rec) != 0) {
            fprintf(stderr, "prolapse synthetic: JSONL write failed\n");
            rc = 2;
            break;
        }
        rows++;
        hist[outcome]++;
        if (outcome == PROLAPSE_OUTCOME_BOTH_ZERO)
            both_zero++;
    }

    printf("[synthetic] histogram both_zero=%llu esrch_main=%llu "
           "esrch_racer=%llu both_esrch=%llu timeout=%llu other=%llu\n",
           (unsigned long long)hist[PROLAPSE_OUTCOME_BOTH_ZERO],
           (unsigned long long)hist[PROLAPSE_OUTCOME_ESRCH_MAIN],
           (unsigned long long)hist[PROLAPSE_OUTCOME_ESRCH_RACER],
           (unsigned long long)hist[PROLAPSE_OUTCOME_BOTH_ESRCH],
           (unsigned long long)hist[PROLAPSE_OUTCOME_TIMEOUT],
           (unsigned long long)hist[PROLAPSE_OUTCOME_OTHER]);
    if (prolapse_write_control(out, "synthetic", rows, both_zero, 0,
                               both_zero == 0,
                               "classification-only rows; no syscalls") != 0)
        rc = 2;
    if (rc == 0 && both_zero != 0 && !cfg->synthetic_no_abort) {
        fprintf(stderr,
                "synthetic: BOTH_ZERO ABORT: %llu both_zero of %llu rows\n",
                (unsigned long long)both_zero, (unsigned long long)rows);
        rc = 1;
    }

    if (in != stdin)
        fclose(in);
    if (out != stdout)
        fclose(out);
    printf("[synthetic] rows=%llu both_zero=%llu abort=%s\n",
           (unsigned long long)rows, (unsigned long long)both_zero,
           rc == 1 ? "true" : "false");
    return rc;
}

#endif /* PROLAPSE_HOST */
