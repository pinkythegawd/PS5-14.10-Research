#include "prolapse.h"

#include <errno.h>
#include <string.h>

void prolapse_json_escape(FILE *out, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!s) {
        fputs("", out);
        return;
    }
    for (; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\b': fputs("\\b", out); break;
        case '\f': fputs("\\f", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 0x20)
                fprintf(out, "\\u%04x", (unsigned)*p);
            else
                fputc(*p, out);
            break;
        }
    }
}

static void json_str_field(FILE *out, const char *key, const char *value)
{
    fprintf(out, ",\"%s\":\"", key);
    prolapse_json_escape(out, value);
    fputc('"', out);
}

static const char *clock_src(void)
{
#ifdef PROLAPSE_HOST
    return "libc";
#else
    return "232";
#endif
}

int prolapse_write_attempt(FILE *out, const prolapse_attempt_record_t *r)
{
    if (!out || !r)
        return -1;

    fprintf(out,
        "{\"record\":\"attempt\",\"run_id\":%llu,\"seq\":%llu,"
        "\"round\":%llu,\"round_id\":%llu",
        (unsigned long long)r->run_id, (unsigned long long)r->seq,
        (unsigned long long)r->round, (unsigned long long)r->round_id);
    json_str_field(out, "strategy", r->strategy ? r->strategy : "unknown");
    fprintf(out,
        ",\"id\":%u,\"id_hex\":\"0x%08x\",\"slot\":%u,\"batch_size\":%u,"
        "\"t_start_us\":%llu,\"t_end_us\":%llu,\"window_us\":%llu,"
        "\"parked_live\":%d,"
        "\"main_ret\":%d,\"main_err\":%d,\"main_err_hex\":\"0x%08x\","
        "\"racer_ret\":%d,\"racer_err\":%d,\"racer_err_hex\":\"0x%08x\","
        "\"main_seq\":%d,\"racer_seq\":%d",
        (unsigned)r->id, (unsigned)r->id, (unsigned)r->slot,
        (unsigned)r->batch_size,
        (unsigned long long)r->t_start_us,
        (unsigned long long)r->t_end_us,
        (unsigned long long)r->window_us, r->parked_live,
        r->main_ret, r->main_err, (unsigned)r->main_err,
        r->racer_ret, r->racer_err, (unsigned)r->racer_err,
        r->main_seq, r->racer_seq);
    json_str_field(out, "outcome", prolapse_outcome_name(r->outcome));
    if (r->note && *r->note)
        json_str_field(out, "note", r->note);
    fprintf(out,
        ",\"frozen\":%s,\"candidate\":%s,\"control\":%s,\"synthetic\":%s,"
        "\"clock_src\":\"%s\",\"sys624_unverified_on_console\":true,"
        "\"sys624_kind\":\"vm_operation_counter_ordering_token\","
        "\"sys624_main_before\":%llu,\"sys624_main_after\":%llu,"
        "\"sys624_racer_before\":%llu,\"sys624_racer_after\":%llu}\n",
        r->frozen ? "true" : "false",
        r->candidate ? "true" : "false",
        r->control ? "true" : "false",
        r->synthetic ? "true" : "false",
        clock_src(),
        (unsigned long long)r->sys624_main_before,
        (unsigned long long)r->sys624_main_after,
        (unsigned long long)r->sys624_racer_before,
        (unsigned long long)r->sys624_racer_after);
    fflush(out);
    return ferror(out) ? -1 : 0;
}

int prolapse_write_control(FILE *out, const char *name, uint64_t attempts,
                           uint64_t both_zero, uint64_t expected, int pass,
                           const char *detail)
{
    if (!out || !name)
        return -1;
    fputs("{\"record\":\"control\"", out);
    json_str_field(out, "control", name);
    fprintf(out, ",\"attempts\":%llu,\"both_zero\":%llu,\"expected\":%llu,"
                 "\"pass\":%s,\"clock_src\":\"%s\"",
            (unsigned long long)attempts, (unsigned long long)both_zero,
            (unsigned long long)expected, pass ? "true" : "false",
            clock_src());
    if (detail)
        json_str_field(out, "detail", detail);
    fputs("}\n", out);
    fflush(out);
    return ferror(out) ? -1 : 0;
}

int prolapse_append_dead_end(const char *path, const char *strategy,
                             uint64_t attempts, uint64_t wins,
                             uint64_t p99_window_us, uint64_t cap_hits,
                             const char *detail)
{
    FILE *f;
    long size;
    if (!path || !strategy)
        return -1;
    f = fopen(path, "a+");
    if (!f) {
        fprintf(stderr, "prolapse: cannot open dead-end ledger %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size == 0) {
        fputs("# Dead ends — Prolapse P1-P3\n\n", f);
        fputs("Measured negative schedules. No guesses.\n\n", f);
    }
    fprintf(f, "## %s\n\n", strategy);
    fprintf(f, "- strategy: `%s`\n", strategy);
    fprintf(f, "- attempts: %llu\n", (unsigned long long)attempts);
    fprintf(f, "- wins: %llu\n", (unsigned long long)wins);
    fprintf(f, "- p99_window_us: %llu\n",
            (unsigned long long)p99_window_us);
    fprintf(f, "- cap_hits: %llu\n", (unsigned long long)cap_hits);
    if (detail && *detail)
        fprintf(f, "- detail: %s\n", detail);
    fputc('\n', f);
    fflush(f);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
