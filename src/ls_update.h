/* Update check and signed download from GitHub releases. */
#ifndef LS_UPDATE_H
#define LS_UPDATE_H

#include <stddef.h>
#include "ls_https.h"

typedef struct {
    char tag[48];          /* e.g. "v0.2" */
    char version[48];      /* e.g. "0.2" */
    char notes[1200];      /* release description */
    char zip_url[700];
    char sig_url[700];
} ls_release;

/* Compares dotted versions ("v1.10" > "v1.9"). Returns <0, 0, >0. */
int ls_version_cmp(const char *a, const char *b);

/* Fetches {api_base}/repos/{repo}/releases/latest and finds `asset` (and asset + ".sig").
 * Returns 0 on success, <0 with a readable reason in err. Compare versions yourself. */
int ls_update_check(const char *api_base, const char *repo, const char *asset, const char *ca_file,
                    ls_release *out, char *err, size_t es);

/* Downloads the zip and its signature, verifies the signature against the built-in
 * public key, and only then writes the zip to dest. */
int ls_update_download(const ls_release *r, const char *ca_file, const char *dest,
                       ls_dl_progress prog, void *ud, char *err, size_t es);

#endif
