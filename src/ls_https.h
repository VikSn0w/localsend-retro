/* Verified HTTPS GET for update checks (Mac OS X only; needs the TLS build). */
#ifndef LS_HTTPS_H
#define LS_HTTPS_H

#include <stddef.h>

typedef void (*ls_dl_progress)(void *ud, long long done, long long total);

/* GET https://... following up to 5 redirects. The server certificate is verified against
 * the CA bundle in ca_file. On success *body is malloc'd (NUL-terminated), *len its size and
 * *status the final HTTP status. Returns 0 on success, <0 on failure with a reason in err. */
int ls_https_get(const char *url, const char *ca_file, long max_bytes, ls_dl_progress prog, void *ud,
                 char **body, long *len, int *status, char *err, size_t es);

#endif
