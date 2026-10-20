#include "ls_update.h"
#include "ls_json.h"
#include "ls_tls.h"
#include "ls_compat.h"
#include "update_key.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_err(char *err, size_t es, const char *msg)
{
    if (es) {
        strncpy(err, msg, es - 1);
        err[es - 1] = 0;
    }
}

static void copy(char *dst, size_t n, const char *src)
{
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = 0;
}

/* next numeric component of a version string, skipping a leading "v" */
static int next_num(const char **p)
{
    int v = 0;
    while (**p && (**p < '0' || **p > '9')) (*p)++;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v;
}

int ls_version_cmp(const char *a, const char *b)
{
    int i;
    for (i = 0; i < 4; i++) {
        int x = next_num(&a), y = next_num(&b);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

int ls_update_check(const char *api_base, const char *repo, const char *asset, const char *ca_file,
                    ls_release *out, char *err, size_t es)
{
    char url[512], want_sig[200];
    char *body = NULL;
    long len = 0;
    int status = 0, i, rc = -1, found = 0, found_sig = 0;
    jval *root = NULL, *assets;

    memset(out, 0, sizeof *out);
    snprintf(url, sizeof url, "%s/repos/%s/releases/latest", api_base, repo);
    snprintf(want_sig, sizeof want_sig, "%s.sig", asset);
    if (ls_https_get(url, ca_file, 2 * 1024 * 1024, NULL, NULL, &body, &len, &status, err, es) != 0) return -1;
    if (status == 404) { set_err(err, es, "no release published yet"); goto out; }
    if (status != 200) {
        snprintf(err, es, "GitHub answered with HTTP %d", status);
        goto out;
    }
    root = json_parse(body);
    if (!root || root->t != J_OBJ) { set_err(err, es, "unreadable reply from GitHub"); goto out; }
    copy(out->tag, sizeof out->tag, json_str(root, "tag_name", ""));
    copy(out->notes, sizeof out->notes, json_str(root, "body", ""));
    if (!out->tag[0]) { set_err(err, es, "release has no version tag"); goto out; }
    {
        const char *v = out->tag;
        if (*v == 'v' || *v == 'V') v++;
        copy(out->version, sizeof out->version, v);
    }
    assets = json_get(root, "assets");
    if (!assets || assets->t != J_ARR) { set_err(err, es, "release has no files attached"); goto out; }
    for (i = 0; i < assets->n; i++) {
        const char *name = json_str(assets->items[i], "name", "");
        const char *u = json_str(assets->items[i], "browser_download_url", "");
        if (!strcmp(name, asset)) { copy(out->zip_url, sizeof out->zip_url, u); found = 1; }
        else if (!strcmp(name, want_sig)) { copy(out->sig_url, sizeof out->sig_url, u); found_sig = 1; }
    }
    if (!found || !found_sig) {
        snprintf(err, es, "release %s has no %s (with .sig) for this Mac", out->tag, asset);
        goto out;
    }
    rc = 0;
out:
    json_free(root);
    free(body);
    return rc;
}

int ls_update_download(const ls_release *r, const char *ca_file, const char *dest,
                       ls_dl_progress prog, void *ud, char *err, size_t es)
{
    char *zip = NULL, *sig = NULL;
    long zl = 0, sl = 0;
    int st = 0, rc = -1;
    FILE *f;

    if (ls_https_get(r->sig_url, ca_file, 4096, NULL, NULL, &sig, &sl, &st, err, es) != 0) goto out;
    if (st != 200 || sl < 8 || sl > 200) { set_err(err, es, "signature file is missing or invalid"); goto out; }
    if (ls_https_get(r->zip_url, ca_file, 24 * 1024 * 1024, prog, ud, &zip, &zl, &st, err, es) != 0) goto out;
    if (st != 200 || zl < 1024) { set_err(err, es, "download failed"); goto out; }

    if (ls_tls_verify_signature(ls_update_pubkey, sizeof ls_update_pubkey, zip, (size_t)zl,
                                (const unsigned char *)sig, (size_t)sl) != 0) {
        set_err(err, es, "the download's signature is not valid, update rejected");
        goto out;
    }
    f = fopen(dest, "wb");
    if (!f) { set_err(err, es, "cannot save the update"); goto out; }
    if (fwrite(zip, 1, (size_t)zl, f) != (size_t)zl) { fclose(f); set_err(err, es, "cannot save the update"); goto out; }
    fclose(f);
    rc = 0;
out:
    free(zip);
    free(sig);
    return rc;
}
