/* Small libc replacements so the core builds with MPW/CodeWarrior libraries too. */
#ifndef LS_COMPAT_H
#define LS_COMPAT_H

#include <stddef.h>

int ls_strcasecmp(const char *a, const char *b);
int ls_strncasecmp(const char *a, const char *b, size_t n);
const char *ls_strcasestr(const char *hay, const char *needle);
long long ls_atoll(const char *s);              /* decimal */
long long ls_hextoll(const char *s);            /* hex prefix, stops at first non-hex digit */
int ls_snprintf(char *dst, size_t n, const char *fmt, ...);   /* no %lld/%zu: use ls_lltoa */
void ls_lltoa(long long v, char *out);          /* out must hold 24 bytes */

#endif
