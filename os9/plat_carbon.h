/* Helpers of the Carbon (Mac OS 9 / Mac OS X PowerPC) backend that the application uses directly. */
#ifndef LS_PLAT_CARBON_H
#define LS_PLAT_CARBON_H

/* The folder where received files are saved, as the opaque "directory" string the core hands back to the
 * backend (it looks like "#<volume>:<folder id>"). Returns 0 on success. */
int plat_carbon_desktop_dir(char *out, int size);

/* This Mac's IP address as dotted text, or "" when it has none. Returns 0 on success. */
int plat_carbon_local_ip(char *out, int size);

#endif
