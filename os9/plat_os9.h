/* Mac OS 9 helpers used by the Classic application (not by the portable core). */
#ifndef PLAT_OS9_H
#define PLAT_OS9_H

#include <Files.h>

/* Initialise Open Transport and the Thread Manager. Returns 0 on success. */
int plat_os9_init(void);

/* Full HFS path ("Disk:Desktop Folder:") of the Desktop folder, with trailing colon. */
int plat_os9_desktop_path(char *out, int size);

/* Full HFS path of a file or folder described by an FSSpec (folders get no trailing colon). */
int plat_os9_spec_to_path(const FSSpec *spec, char *out, int size);

/* Shows this Mac's IP address as dotted text. Returns 0 on success. */
int plat_os9_local_ip(char *out, int size);

#endif
