/* Platform layer used by the LocalSend core (ls.c).
 * Backends: plat_posix.c (Mac OS X, BSD sockets) and plat_os9.c (Mac OS 9,
 * Open Transport + File Manager + Thread Manager). Written in plain C89. */
#ifndef LS_PLAT_H
#define LS_PLAT_H

#include <stddef.h>

typedef struct plat_conn plat_conn;           /* one TCP connection */
typedef struct plat_listener plat_listener;   /* listening TCP socket */
typedef struct plat_udp plat_udp;             /* UDP socket joined to the multicast group */
typedef struct plat_file plat_file;
typedef void (*plat_thread_fn)(void *arg);

void plat_init(void);

/* time and randomness */
double plat_now(void);                        /* seconds, monotonic enough for timeouts */
unsigned long plat_rand32(void);

/* threads (cooperative on Mac OS 9) and locking */
void *plat_mutex_new(void);
void plat_mutex_lock(void *m);
void plat_mutex_unlock(void *m);
/* Starts fn(arg) in a new thread. If handle is not NULL it receives a handle
 * for plat_thread_join, otherwise the thread is detached. Returns 0 on success. */
int plat_thread_start(plat_thread_fn fn, void *arg, void **handle);
void plat_thread_join(void *handle);

/* UDP: bind to port, join group (dotted quad). NULL on failure. */
plat_udp *plat_udp_open(int port, const char *group);
int plat_udp_send(plat_udp *u, const char *ip, int port, const void *buf, size_t n);
/* Returns bytes received, 0 on timeout, <0 on error. ip_out holds 48 bytes. */
long plat_udp_recv(plat_udp *u, void *buf, size_t n, char *ip_out, int timeout_ms);
void plat_udp_close(plat_udp *u);

/* TCP */
plat_listener *plat_tcp_listen(int port);
/* NULL on timeout. ip_out holds 48 bytes. */
plat_conn *plat_tcp_accept(plat_listener *l, char *ip_out, int timeout_ms);
void plat_listener_close(plat_listener *l);
plat_conn *plat_tcp_connect(const char *ip, int port, int timeout_s);
long plat_conn_read(plat_conn *c, void *buf, size_t n);          /* >0, 0 EOF, <0 error */
long plat_conn_write(plat_conn *c, const void *buf, size_t n);   /* bytes written or <0 */
/* First pending byte without consuming it: 0..255, -1 on error, 256 if unsupported. */
int plat_conn_peek(plat_conn *c);
int plat_conn_fd(plat_conn *c);                                  /* -1 where there is no fd */
void plat_conn_close(plat_conn *c);

/* files; path strings are native (POSIX paths, or HFS "Disk:Folder:File" paths) */
void plat_mkdir(const char *dir);
/* Creates dir/name without overwriting (adds " (n)" when it exists). */
plat_file *plat_file_create_unique(const char *dir, const char *name, char *path_out, size_t ps);
long plat_file_write(plat_file *f, const void *buf, size_t n);
void plat_file_close(plat_file *f);
void plat_file_remove(const char *path);
/* Closes f and deletes the half-written file it points to (path is what create_unique returned). */
void plat_file_abort(plat_file *f, const char *path);
plat_file *plat_file_open_read(const char *path);
long plat_file_read(plat_file *f, void *buf, size_t n);
int plat_file_size(const char *path, long long *size);           /* 0 on success */
const char *plat_basename(const char *path);

#endif
