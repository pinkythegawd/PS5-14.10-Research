#include "prolapse.h"

/*
 * PS5 AIO errors are reported in the per-id int array.  The generation check
 * in docs/AIO_BATCH_14_10.md produced 0x80020003; the FreeBSD numeric ESRCH is
 * 3.  Both spellings are accepted, and nothing else is treated as "gone".
 */
int prolapse_is_esrch(int err)
{
    uint32_t u = (uint32_t)err;
    return u == 3u || u == 0x80020003u;
}

int prolapse_is_zero(int err)
{
    return err == 0;
}

prolapse_outcome_t prolapse_classify(int main_ret, int racer_ret,
                                     int main_err, int racer_err)
{
    (void)main_ret;
    (void)racer_ret;

    if (prolapse_is_zero(main_err) && prolapse_is_zero(racer_err))
        return PROLAPSE_OUTCOME_BOTH_ZERO;

    if (prolapse_is_esrch(main_err) && prolapse_is_esrch(racer_err))
        return PROLAPSE_OUTCOME_BOTH_ESRCH;

    if (prolapse_is_esrch(main_err))
        return PROLAPSE_OUTCOME_ESRCH_MAIN;

    if (prolapse_is_esrch(racer_err))
        return PROLAPSE_OUTCOME_ESRCH_RACER;

    return PROLAPSE_OUTCOME_OTHER;
}

const char *prolapse_outcome_name(prolapse_outcome_t outcome)
{
    switch (outcome) {
    case PROLAPSE_OUTCOME_BOTH_ZERO:  return "both_zero";
    case PROLAPSE_OUTCOME_ESRCH_MAIN: return "esrch_main";
    case PROLAPSE_OUTCOME_ESRCH_RACER:return "esrch_racer";
    case PROLAPSE_OUTCOME_BOTH_ESRCH: return "both_esrch";
    case PROLAPSE_OUTCOME_TIMEOUT:    return "timeout";
    case PROLAPSE_OUTCOME_OTHER:      return "other";
    default:                          return "other";
    }
}
