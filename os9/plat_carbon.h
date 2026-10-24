/* Helpers of the Carbon (Mac OS 9 / Mac OS X PowerPC) backend that the application uses directly. */
#ifndef LS_PLAT_CARBON_H
#define LS_PLAT_CARBON_H

/* The folder where received files are saved, as the opaque "directory" string the core hands back to the
 * backend (it looks like "#<volume>:<folder id>"). Returns 0 on success. */
int plat_carbon_desktop_dir(char *out, int size);

/* This Mac's IP address as dotted text, or "" when it has none. Returns 0 on success. */
int plat_carbon_local_ip(char *out, int size);

/* A small text setting kept in the Preferences folder (the user's own on Mac OS X, the System Folder's on Mac OS 9).
 * read returns the text length (0 if there is none yet), write returns 0 on success. */
int plat_carbon_read_pref(const char *name, char *out, int size);
int plat_carbon_write_pref(const char *name, const char *text);

#endif
