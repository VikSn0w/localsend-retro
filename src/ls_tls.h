/* Thin TLS wrapper around mbedTLS. Without -DLS_WITH_TLS every call fails
 * and the core runs in plain-HTTP mode only. */
#ifndef LS_TLS_H
#define LS_TLS_H

#include <stddef.h>

typedef struct ls_tls ls_tls;

int ls_tls_available(void);                 /* 1 only once ls_tls_init succeeded */
const char *ls_tls_error(void);             /* why TLS is not available */

/* Loads (or creates and stores in data_dir) the device certificate.
 * data_dir may be NULL for a throw-away certificate.
 * fp_out receives the lowercase hex SHA-256 of the certificate (65 bytes). */
int ls_tls_init(const char *data_dir, char *fp_out);

ls_tls *ls_tls_accept(int fd);    /* server side handshake */
ls_tls *ls_tls_connect(int fd);   /* client side, no certificate verification */
int ls_tls_read(ls_tls *t, void *buf, size_t n);          /* >0 bytes, 0 EOF, <0 error */
int ls_tls_write(ls_tls *t, const void *buf, size_t n);   /* bytes written or <0 */
void ls_tls_close(ls_tls *t);
void ls_tls_info(ls_tls *t, char *out, size_t n);   /* "TLSv1.3 TLS1-3-CHACHA20-POLY1305-SHA256" */

/* Verified client session (for downloading updates): checks the server certificate
 * against the CA bundle in ca_file and the host name. On failure returns NULL and
 * writes a readable reason into err. */
ls_tls *ls_tls_connect_verified(int fd, const char *host, const char *ca_file, char *err, size_t es);

/* ECDSA P-256 / SHA-256: 0 if sig (DER) is valid for data under pub (DER SubjectPublicKeyInfo). */
int ls_tls_verify_signature(const unsigned char *pub, size_t publen, const void *data, size_t n,
                            const unsigned char *sig, size_t siglen);

#endif
