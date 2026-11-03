#include <kos.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

int gex_portable_main(int argc, char **argv);

int main(int argc, char **argv) {
    arch_set_exit_path(ARCH_EXIT_MENU);
    fprintf(stderr, "[dreamcast] Gex SH4 host (16 MiB RAM)\n");
    if (argc > 1) return gex_portable_main(argc, argv);
    FILE *config = fopen("/cd/gex-test.cfg", "r");
    if (config) {
        static char args[1024];
        static char *test_argv[32] = {"gex"};
        int valid = fgets(args, sizeof args, config) && strchr(args, '\n');
        if (fclose(config)) valid = 0;
        if (!valid) {
            fprintf(stderr, "[dreamcast] invalid test arguments\n");
            return 64;
        }
        int count = 1;
        char *save, *token = strtok_r(args, " \r\n\t", &save);
        while (token && count < 31) {
            test_argv[count++] = token;
            token = strtok_r(NULL, " \r\n\t", &save);
        }
        if (token) {
            fprintf(stderr, "[dreamcast] too many test arguments\n");
            return 64;
        }
        test_argv[count] = NULL;
        return gex_portable_main(count, test_argv);
    }
    if (errno != ENOENT) {
        fprintf(stderr, "[dreamcast] cannot read test arguments: %s\n", strerror(errno));
        return 1;
    }
    char *defaults[] = {"gex", "--data", "/cd/Gex", NULL};
    return gex_portable_main(3, defaults);
}
