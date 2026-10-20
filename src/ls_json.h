/* Minimal JSON parser (C99, no dependencies). */
#ifndef LS_JSON_H
#define LS_JSON_H

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
    jtype t;
    double num;
    int b;
    char *s;
    struct jval **items;   /* array elements or object values */
    char **keys;           /* object keys (parallel to items) */
    int n, cap;
} jval;

jval *json_parse(const char *s);
void json_free(jval *v);
jval *json_get(const jval *obj, const char *key);
const char *json_str(const jval *obj, const char *key, const char *def);
double json_num(const jval *obj, const char *key, double def);

#endif
