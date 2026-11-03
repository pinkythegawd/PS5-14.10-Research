#include "prolapse.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    prolapse_config_t cfg;
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (prolapse_config_load(&cfg, argc, argv) != 0)
        return 2;
    if (cfg.stage && strcmp(cfg.stage, "help") == 0)
        return 0;
    if (cfg.list_config) {
        prolapse_config_print(&cfg, stdout);
        return 0;
    }

    printf("prolapse: stage=%s profile=%s strategy=%s rounds=%s batch=%s "
           "max_threads=%s stop_on_first_win=%s\n",
           cfg.stage,
           prolapse_config_get(&cfg, "target.profile"),
           prolapse_config_get(&cfg, "race.strategy"),
           prolapse_config_get(&cfg, "race.rounds"),
           prolapse_config_get(&cfg, "race.batch"),
           prolapse_config_get(&cfg, "race.max_threads"),
           prolapse_config_get(&cfg, "race.stop_on_first_win"));

    if (strcmp(cfg.stage, "p1") == 0)
        rc = prolapse_p1_run(&cfg);
    else
        rc = prolapse_p2_run(&cfg);

    printf("prolapse: stage=%s exit=%d\n", cfg.stage, rc);
    return rc == 0 ? 0 : 1;
}
