/* Command-line test tool for the LocalSend core. Auto-accepts incoming files. */
#include "ls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void on_peer(void *ud, const ls_peer *p)
{
    (void)ud;
    printf("[peer] %s  %s:%d  %s  (%s)\n", p->alias, p->ip, p->port, p->protocol, p->deviceModel);
}

static int on_incoming(void *ud, const char *from, int n, const ls_file *f)
{
    int i;
    (void)ud;
    printf("[incoming] %s wants to send %d file(s):\n", from, n);
    for (i = 0; i < n; i++) printf("   %s (%lld bytes)\n", f[i].name, f[i].size);
    return 1;
}

static void on_progress(void *ud, const char *name, long long done, long long total, int sending)
{
    (void)ud;
    printf("\r[%s] %s %lld/%lld", sending ? "send" : "recv", name, done, total);
    if (done >= total) printf("\n");
    fflush(stdout);
}

static void on_received(void *ud, const char *path) { (void)ud; printf("[saved] %s\n", path); }
static void on_log(void *ud, const char *m) { (void)ud; printf("[log] %s\n", m); }

int main(int argc, char **argv)
{
    const char *alias = "PPC-CLI", *dir = "./received", *data = NULL;
    int port = LS_PORT, i;
    ls_callbacks cb = { 0, on_peer, on_incoming, on_progress, on_received, on_log };
    ls_ctx *c;
    char line[1024];

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-a")) alias = argv[i + 1];
        else if (!strcmp(argv[i], "-d")) dir = argv[i + 1];
        else if (!strcmp(argv[i], "-t")) data = argv[i + 1];   /* cert dir: enables HTTPS */
        else if (!strcmp(argv[i], "-p")) port = atoi(argv[i + 1]);
    }
    c = ls_start(alias, dir, data, port, &cb);
    if (!c) return 1;
    printf("commands: list | announce | send <peer#> <file> | sendto <alias> <file> | quit\n");
    while (fgets(line, sizeof line, stdin)) {
        char cmd[16] = "", path[900] = "", who[64] = "";
        int idx = -1;
        if (!strncmp(line, "sendto ", 7)) {
            ls_peer ps[LS_MAX_PEERS];
            int n, k;
            const char *paths[1];
            sscanf(line + 7, "%63s %899[^\n]", who, path);
            n = ls_peer_list(c, ps, LS_MAX_PEERS);
            for (k = 0; k < n; k++)
                if (!strcmp(ps[k].alias, who)) {
                    paths[0] = path;
                    printf("result: %d\n", ls_send(c, &ps[k], 1, paths));
                    break;
                }
            if (k == n) printf("no peer named %s\n", who);
            continue;
        }
        sscanf(line, "%15s %d %899[^\n]", cmd, &idx, path);
        if (!strcmp(cmd, "quit")) break;
        if (!strcmp(cmd, "announce")) ls_announce(c);
        else if (!strcmp(cmd, "list")) {
            ls_peer ps[LS_MAX_PEERS];
            int n = ls_peer_list(c, ps, LS_MAX_PEERS);
            for (i = 0; i < n; i++)
                printf("%d: %s  %s:%d  %s\n", i, ps[i].alias, ps[i].ip, ps[i].port, ps[i].protocol);
        } else if (!strcmp(cmd, "send") && idx >= 0 && path[0]) {
            ls_peer ps[LS_MAX_PEERS];
            int n = ls_peer_list(c, ps, LS_MAX_PEERS);
            const char *paths[1];
            paths[0] = path;
            if (idx < n) printf("result: %d\n", ls_send(c, &ps[idx], 1, paths));
            else printf("no such peer\n");
        }
    }
    ls_stop(c);
    return 0;
}
