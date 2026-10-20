/* Test tool: ls-updtest <api_base> <repo> <asset> <ca_file> <current_version> [download <dest.zip>] */
#include "ls_update.h"
#include <stdio.h>
#include <string.h>

static void prog(void *ud, long long done, long long total)
{
    (void)ud;
    printf("\r  %lld / %lld bytes", done, total);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    ls_release r;
    char err[300];
    if (argc == 4 && !strcmp(argv[1], "get")) {      /* ls-updtest get <url> <ca_file> */
        char *body;
        long len;
        int st;
        if (ls_https_get(argv[2], argv[3], 8 * 1024 * 1024, NULL, NULL, &body, &len, &st, err, sizeof err) != 0) {
            printf("failed: %s\n", err);
            return 1;
        }
        printf("HTTP %d, %ld bytes\n%.200s\n", st, len, body);
        return 0;
    }
    if (argc < 6) {
        fprintf(stderr, "usage: %s api_base repo asset ca_file current [download dest]\n", argv[0]);
        return 2;
    }
    if (ls_update_check(argv[1], argv[2], argv[3], argv[4], &r, err, sizeof err) != 0) {
        printf("check failed: %s\n", err);
        return 1;
    }
    printf("latest: %s (version %s)\nnewer than %s: %s\nnotes: %.80s\nzip: %s\nsig: %s\n", r.tag, r.version,
           argv[5], ls_version_cmp(r.version, argv[5]) > 0 ? "yes" : "no", r.notes, r.zip_url, r.sig_url);
    if (argc >= 8 && !strcmp(argv[6], "download")) {
        if (ls_update_download(&r, argv[4], argv[7], prog, NULL, err, sizeof err) != 0) {
            printf("\ndownload failed: %s\n", err);
            return 1;
        }
        printf("\ndownloaded and verified -> %s\n", argv[7]);
    }
    return 0;
}
