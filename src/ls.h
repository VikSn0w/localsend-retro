/* LocalSend v2 protocol core (HTTP mode). Portable C99 + POSIX + pthreads. */
#ifndef LS_H
#define LS_H

#define LS_PORT 53317
#define LS_MAX_PEERS 64

typedef struct {
    char alias[64];
    char ip[48];
    char fingerprint[72];
    char protocol[8];       /* "http" or "https" */
    char deviceType[16];
    char deviceModel[64];
    int port;
} ls_peer;

typedef struct {
    char id[64];
    char name[256];
    long long size;
} ls_file;

/* All callbacks are invoked from background threads. */
typedef struct {
    void *ud;
    /* a peer was discovered or updated */
    void (*peer)(void *ud, const ls_peer *p);
    /* incoming transfer request; return non-zero to accept. May block. */
    int (*incoming)(void *ud, const char *from, int nfiles, const ls_file *files);
    /* transfer progress (throttled) */
    void (*progress)(void *ud, const char *name, long long done, long long total, int sending);
    /* a file was fully received and stored at path */
    void (*received)(void *ud, const char *path);
    void (*log)(void *ud, const char *msg);
} ls_callbacks;

typedef struct ls_ctx ls_ctx;

/* port: TCP listen port (normally LS_PORT).
 * data_dir: where the device certificate is kept; enables HTTPS. NULL = plain HTTP only.
 * Returns NULL on failure. */
ls_ctx *ls_start(const char *alias, const char *download_dir, const char *data_dir, int port,
                  const ls_callbacks *cb);
void ls_stop(ls_ctx *c);

/* Change the name other devices see, and where received files are saved, while running.
 * ls_set_alias announces the new name right away. */
void ls_set_alias(ls_ctx *c, const char *alias);
void ls_set_download_dir(ls_ctx *c, const char *dir);

/* Announce https (on) or plain http (off) to other devices. Off is faster on old Macs and keeps
 * working with every peer; incoming TLS connections are still accepted either way. */
void ls_set_encryption(ls_ctx *c, int on);

/* Whether HTTPS (sending to encrypted devices) works, and if not, why. */
int ls_https_available(void);
const char *ls_https_error(void);

/* Multicast an announcement so other devices register with us. */
void ls_announce(ls_ctx *c);

/* Copies up to max known peers into out; returns the count. */
int ls_peer_list(ls_ctx *c, ls_peer *out, int max);

/* Sends files to a peer (blocking). Returns 0 on success, <0 on error. */
int ls_send(ls_ctx *c, const ls_peer *p, int n, const char **paths);

#endif
