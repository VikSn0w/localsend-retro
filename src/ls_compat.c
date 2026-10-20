#include "ls_compat.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int ls_strcasecmp(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int ls_strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
        n--;
    }
    if (!n) return 0;
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

const char *ls_strcasestr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (; *hay; hay++)
        if (ls_strncasecmp(hay, needle, nl) == 0) return hay;
    return NULL;
}

long long ls_atoll(const char *s)
{
    long long v = 0;
    int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}

long long ls_hextoll(const char *s)
{
    long long v = 0;
    for (;; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * 16 + d;
    }
    return v;
}

void ls_lltoa(long long v, char *out)
{
    char tmp[24];
    int i = 0, neg = 0, j = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) {
        tmp[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    if (neg) out[j++] = '-';
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

int ls_snprintf(char *dst, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (!n) return 0;
#ifdef LS_NO_VSNPRINTF
    {
        char tmp[2048];
        va_start(ap, fmt);
        r = vsprintf(tmp, fmt, ap);      /* callers keep output well below 2 KB */
        va_end(ap);
        strncpy(dst, tmp, n - 1);
        dst[n - 1] = 0;
    }
#else
    va_start(ap, fmt);
    r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
#endif
    return r;
}
