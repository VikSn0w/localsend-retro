#include "ls_tls.h"
#include <string.h>

#ifndef LS_WITH_TLS

int ls_tls_available(void) { return 0; }
const char *ls_tls_error(void) { return "this build has no TLS support"; }
int ls_tls_init(const char *d, char *fp) { (void)d; (void)fp; return -1; }
ls_tls *ls_tls_accept(int fd) { (void)fd; return NULL; }
ls_tls *ls_tls_connect(int fd) { (void)fd; return NULL; }
int ls_tls_read(ls_tls *t, void *b, size_t n) { (void)t; (void)b; (void)n; return -1; }
int ls_tls_write(ls_tls *t, const void *b, size_t n) { (void)t; (void)b; (void)n; return -1; }
void ls_tls_close(ls_tls *t) { (void)t; }
ls_tls *ls_tls_connect_verified(int fd, const char *host, const char *ca, char *err, size_t es)
{
    (void)fd; (void)host; (void)ca;
    if (es) strncpy(err, "this build has no TLS support", es - 1), err[es - 1] = 0;
    return NULL;
}
int ls_tls_verify_signature(const unsigned char *k, size_t kl, const void *d, size_t n,
                            const unsigned char *sg, size_t sl)
{
    (void)k; (void)kl; (void)d; (void)n; (void)sg; (void)sl;
    return -1;
}

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/sha256.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"

struct ls_tls {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt ca;       /* only for verified client sessions */
    int has_ca;
};

static mbedtls_pk_context g_key;
static mbedtls_x509_crt g_crt;
static int g_ready;
static const char *g_why = "not initialised";

int ls_tls_available(void) { return g_ready; }
const char *ls_tls_error(void) { return g_why; }

static int read_file(const char *path, unsigned char **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return -1; }
    *buf = (unsigned char *)malloc((size_t)sz);
    if (fread(*buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(*buf); return -1; }
    fclose(f);
    *len = (size_t)sz;
    return 0;
}

static int write_file(const char *path, const unsigned char *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(buf, 1, len, f);
    fclose(f);
    chmod(path, 0600);
    return 0;
}

static int load_pair(const char *kp, const char *cp, mbedtls_ctr_drbg_context *drbg)
{
    unsigned char *kb = NULL, *cb = NULL;
    size_t kl, cl;
    int rc = -1;
    if (read_file(kp, &kb, &kl) || read_file(cp, &cb, &cl)) goto out;
    if (mbedtls_pk_parse_key(&g_key, kb, kl, NULL, 0, mbedtls_ctr_drbg_random, drbg)) goto out;
    if (mbedtls_x509_crt_parse_der(&g_crt, cb, cl)) goto out;
    rc = 0;
out:
    free(kb);
    free(cb);
    return rc;
}

static int make_pair(const char *kp, const char *cp, mbedtls_ctr_drbg_context *drbg)
{
    mbedtls_x509write_cert w;
    unsigned char kder[512], cder[2048], serial[16];
    int kl, cl, rc = -1;

    mbedtls_x509write_crt_init(&w);
    if (mbedtls_pk_setup(&g_key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) goto out;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(g_key), mbedtls_ctr_drbg_random, drbg)) goto out;

    mbedtls_x509write_crt_set_version(&w, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &g_key);
    mbedtls_x509write_crt_set_issuer_key(&w, &g_key);
    if (mbedtls_x509write_crt_set_subject_name(&w, "CN=LocalSend User,O=LocalSend")) goto out;
    if (mbedtls_x509write_crt_set_issuer_name(&w, "CN=LocalSend User,O=LocalSend")) goto out;
    if (mbedtls_ctr_drbg_random(drbg, serial, sizeof serial)) goto out;
    serial[0] &= 0x7F;
    if (mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof serial)) goto out;
    if (mbedtls_x509write_crt_set_validity(&w, "20200101000000", "20500101000000")) goto out;
    if (mbedtls_x509write_crt_set_basic_constraints(&w, 0, -1)) goto out;

    cl = mbedtls_x509write_crt_der(&w, cder, sizeof cder, mbedtls_ctr_drbg_random, drbg);
    if (cl <= 0) goto out;
    kl = mbedtls_pk_write_key_der(&g_key, kder, sizeof kder);
    if (kl <= 0) goto out;
    /* DER is written at the end of the buffer */
    if (kp && cp) {
        write_file(kp, kder + sizeof kder - kl, (size_t)kl);
        write_file(cp, cder + sizeof cder - cl, (size_t)cl);
    }
    if (mbedtls_x509_crt_parse_der(&g_crt, cder + sizeof cder - cl, (size_t)cl)) goto out;
    rc = 0;
out:
    mbedtls_x509write_crt_free(&w);
    return rc;
}

int ls_tls_init(const char *data_dir, char *fp_out)
{
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    char kp[1100], cp[1100];
    unsigned char h[32];
    int rc = -1, i;

    if (g_ready) return 0;
    g_why = "crypto library start-up (psa_crypto_init)";
    if (psa_crypto_init() != PSA_SUCCESS) return -1;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&g_key);
    mbedtls_x509_crt_init(&g_crt);
    g_why = "random number generator";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char *)"ls-ppc", 6)) goto out;
    g_why = "device certificate";

    if (data_dir) {
        mkdir(data_dir, 0700);
        snprintf(kp, sizeof kp, "%s/key.der", data_dir);
        snprintf(cp, sizeof cp, "%s/cert.der", data_dir);
        if (load_pair(kp, cp, &drbg) != 0) {
            mbedtls_pk_free(&g_key); mbedtls_pk_init(&g_key);
            mbedtls_x509_crt_free(&g_crt); mbedtls_x509_crt_init(&g_crt);
            if (make_pair(kp, cp, &drbg)) goto out;
        }
    } else if (make_pair(NULL, NULL, &drbg)) {
        goto out;
    }
    mbedtls_sha256(g_crt.raw.p, g_crt.raw.len, h, 0);
    for (i = 0; i < 32; i++) sprintf(fp_out + 2 * i, "%02x", h[i]);
    g_ready = 1;
    g_why = "";
    rc = 0;
out:
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return rc;
}

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    int fd = *(int *)ctx;
    ssize_t w = send(fd, b, n, 0);
    if (w < 0) return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    return (int)w;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    int fd = *(int *)ctx;
    ssize_t r = recv(fd, b, n, 0);
    if (r < 0) return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
    return (int)r;
}

static ls_tls *open_session(int fd, int server)
{
    ls_tls *t = (ls_tls *)calloc(1, sizeof *t);
    int rc;
    if (!t || !g_ready) { free(t); return NULL; }
    t->fd = fd;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->ent);
    mbedtls_ctr_drbg_init(&t->drbg);
    if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->ent, (const unsigned char *)"ls", 2)) goto fail;
    if (mbedtls_ssl_config_defaults(&t->conf, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) goto fail;
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    /* LocalSend peers use mutual TLS: the client presents its certificate too. */
    if (mbedtls_ssl_conf_own_cert(&t->conf, &g_crt, &g_key)) goto fail;
    if (mbedtls_ssl_setup(&t->ssl, &t->conf)) goto fail;
    mbedtls_ssl_set_bio(&t->ssl, &t->fd, bio_send, bio_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0)
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) goto fail;
    return t;
fail:
    ls_tls_close(t);
    return NULL;
}

ls_tls *ls_tls_accept(int fd) { return open_session(fd, 1); }
ls_tls *ls_tls_connect(int fd) { return open_session(fd, 0); }

int ls_tls_read(ls_tls *t, void *buf, size_t n)
{
    int r;
    do r = mbedtls_ssl_read(&t->ssl, (unsigned char *)buf, n);
    while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
    return r;
}

int ls_tls_write(ls_tls *t, const void *buf, size_t n)
{
    int r;
    do r = mbedtls_ssl_write(&t->ssl, (const unsigned char *)buf, n);
    while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    return r;
}

void ls_tls_close(ls_tls *t)
{
    if (!t) return;
    mbedtls_ssl_close_notify(&t->ssl);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_ctr_drbg_free(&t->drbg);
    mbedtls_entropy_free(&t->ent);
    if (t->has_ca) mbedtls_x509_crt_free(&t->ca);
    free(t);
}

/* Client session that really verifies the server: certificate chain against the CA
 * bundle in ca_file, host name match, SNI sent. Used for update downloads. */
ls_tls *ls_tls_connect_verified(int fd, const char *host, const char *ca_file, char *err, size_t es)
{
    ls_tls *t = (ls_tls *)calloc(1, sizeof *t);
    int rc;
    char msg[160];

    if (!t) return NULL;
    t->fd = fd;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_entropy_init(&t->ent);
    mbedtls_ctr_drbg_init(&t->drbg);
    mbedtls_x509_crt_init(&t->ca);
    t->has_ca = 1;
    if (psa_crypto_init() != PSA_SUCCESS) { strncpy(err, "crypto start-up failed", es - 1); goto fail; }
    if (mbedtls_x509_crt_parse_file(&t->ca, ca_file) < 0 || t->ca.raw.len == 0) {
        strncpy(err, "cannot read the CA certificate bundle", es - 1);
        goto fail;
    }
    if (mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->ent, (const unsigned char *)"lsv", 3)) {
        strncpy(err, "random generator failed", es - 1);
        goto fail;
    }
    if (mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT)) goto fail;
    mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    if (mbedtls_ssl_setup(&t->ssl, &t->conf)) goto fail;
    if (mbedtls_ssl_set_hostname(&t->ssl, host)) goto fail;
    mbedtls_ssl_set_bio(&t->ssl, &t->fd, bio_send, bio_recv, NULL);
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char info[128];
            mbedtls_x509_crt_verify_info(info, sizeof info, "", mbedtls_ssl_get_verify_result(&t->ssl));
            snprintf(err, es, "certificate check failed: %s (is this Mac's date and time right?)", info);
        } else {
            mbedtls_strerror(rc, msg, sizeof msg);
            snprintf(err, es, "secure connection failed: %s", msg);
        }
        mbedtls_ssl_free(&t->ssl);
        mbedtls_ssl_init(&t->ssl);          /* ls_tls_close must not send a close_notify */
        ls_tls_close(t);
        return NULL;
    }
    return t;
fail:
    if (es && !err[0]) strncpy(err, "TLS setup failed", es - 1);
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_init(&t->ssl);
    ls_tls_close(t);
    return NULL;
}

/* ECDSA (P-256, SHA-256) check of data against a DER signature with a DER public key. */
int ls_tls_verify_signature(const unsigned char *pub, size_t publen, const void *data, size_t n,
                            const unsigned char *sig, size_t siglen)
{
    mbedtls_pk_context pk;
    unsigned char h[32];
    int rc;
    if (psa_crypto_init() != PSA_SUCCESS) return -1;
    mbedtls_pk_init(&pk);
    if (mbedtls_pk_parse_public_key(&pk, pub, publen) != 0) { mbedtls_pk_free(&pk); return -1; }
    mbedtls_sha256((const unsigned char *)data, n, h, 0);
    rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, h, sizeof h, sig, siglen);
    mbedtls_pk_free(&pk);
    return rc == 0 ? 0 : -1;
}

#endif
