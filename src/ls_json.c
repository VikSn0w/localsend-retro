#include "ls_json.h"
#include <stdlib.h>
#include <string.h>

static const char *skipws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static jval *jnew(jtype t)
{
    jval *v = (jval *)calloc(1, sizeof(jval));
    if (v) v->t = t;
    return v;
}

static int jpush(jval *v, char *key, jval *c)
{
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        jval **ni = (jval **)realloc(v->items, nc * sizeof(jval *));
        if (!ni) return 0;
        v->items = ni;
        if (v->t == J_OBJ) {
            char **nk = (char **)realloc(v->keys, nc * sizeof(char *));
            if (!nk) return 0;
            v->keys = nk;
        }
        v->cap = nc;
    }
    v->items[v->n] = c;
    if (v->t == J_OBJ) v->keys[v->n] = key;
    v->n++;
    return 1;
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    return v;
}

static char *put_utf8(char *o, unsigned cp)
{
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* Parses a string starting at *pp (must point at the opening quote). */
static char *pstr(const char **pp)
{
    const char *p = *pp, *q;
    char *out, *o;
    if (*p != '"') return NULL;
    p++;
    q = p;
    while (*q && *q != '"') {
        if (*q == '\\' && q[1]) q++;
        q++;
    }
    if (*q != '"') return NULL;
    out = (char *)malloc((size_t)(q - p) + 1);
    if (!out) return NULL;
    o = out;
    while (p < q) {
        if (*p != '\\') { *o++ = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; p++; break;
        case 't': *o++ = '\t'; p++; break;
        case 'r': *o++ = '\r'; p++; break;
        case 'b': *o++ = '\b'; p++; break;
        case 'f': *o++ = '\f'; p++; break;
        case 'u': {
            unsigned cp = hex4(p + 1);
            p += 5;
            if (cp >= 0xD800 && cp < 0xDC00 && p + 5 < q + 1 && p[0] == '\\' && p[1] == 'u') {
                unsigned lo = hex4(p + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            if (p > q) p = q;
            o = put_utf8(o, cp);
            break;
        }
        default: *o++ = *p++; break;   /* \" \\ \/ and unknown */
        }
    }
    *o = 0;
    *pp = q + 1;
    return out;
}

static jval *pval(const char **pp, int depth)
{
    const char *p = skipws(*pp);
    jval *v = NULL;
    if (depth > 32) return NULL;

    if (*p == '{') {
        v = jnew(J_OBJ);
        if (!v) return NULL;
        p = skipws(p + 1);
        if (*p == '}') { *pp = p + 1; return v; }
        for (;;) {
            char *k;
            jval *c;
            p = skipws(p);
            k = pstr(&p);
            if (!k) goto fail;
            p = skipws(p);
            if (*p != ':') { free(k); goto fail; }
            p++;
            c = pval(&p, depth + 1);
            if (!c) { free(k); goto fail; }
            if (!jpush(v, k, c)) { free(k); json_free(c); goto fail; }
            p = skipws(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            goto fail;
        }
    } else if (*p == '[') {
        v = jnew(J_ARR);
        if (!v) return NULL;
        p = skipws(p + 1);
        if (*p == ']') { *pp = p + 1; return v; }
        for (;;) {
            jval *c = pval(&p, depth + 1);
            if (!c) goto fail;
            if (!jpush(v, NULL, c)) { json_free(c); goto fail; }
            p = skipws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; break; }
            goto fail;
        }
    } else if (*p == '"') {
        v = jnew(J_STR);
        if (!v) return NULL;
        v->s = pstr(&p);
        if (!v->s) goto fail;
    } else if (!strncmp(p, "true", 4)) {
        v = jnew(J_BOOL); if (!v) return NULL;
        v->b = 1; p += 4;
    } else if (!strncmp(p, "false", 5)) {
        v = jnew(J_BOOL); if (!v) return NULL;
        v->b = 0; p += 5;
    } else if (!strncmp(p, "null", 4)) {
        v = jnew(J_NULL); if (!v) return NULL;
        p += 4;
    } else {
        char *e;
        double d = strtod(p, &e);
        if (e == p) return NULL;
        v = jnew(J_NUM);
        if (!v) return NULL;
        v->num = d;
        p = e;
    }
    *pp = p;
    return v;
fail:
    json_free(v);
    return NULL;
}

jval *json_parse(const char *s)
{
    const char *p = s;
    if (!s) return NULL;
    return pval(&p, 0);
}

void json_free(jval *v)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->s);
    free(v);
}

jval *json_get(const jval *obj, const char *key)
{
    int i;
    if (!obj || obj->t != J_OBJ) return NULL;
    for (i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

const char *json_str(const jval *obj, const char *key, const char *def)
{
    jval *v = json_get(obj, key);
    return (v && v->t == J_STR && v->s) ? v->s : def;
}

double json_num(const jval *obj, const char *key, double def)
{
    jval *v = json_get(obj, key);
    return (v && v->t == J_NUM) ? v->num : def;
}
