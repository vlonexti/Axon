/* axs_rt.h - the small C runtime used by programs compiled with axs (the self-hosted compiler).
   Strings are immutable views; lists are growable arrays of any element size.
   Like Axon v0.1, this runtime does not free memory. */
#ifndef AXS_RT_H
#define AXS_RT_H
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct axs_str { const char* p; int64_t n; } axs_str;
typedef struct axs_list { char* data; int64_t len; int64_t cap; } axs_list;

static int axs_argc;
static char** axs_argv;

static void axs_fail(const char* msg, int64_t a, int64_t b) {
    fflush(stdout);
    fprintf(stderr, "panic: %s (%lld, %lld)\n", msg, (long long)a, (long long)b);
    exit(101);
}
static axs_str axs_lit(const char* p, int64_t n) { axs_str s; s.p = p; s.n = n; return s; }
static axs_str axs_new(const char* p, int64_t n) {
    char* m = (char*)malloc((size_t)n + 1);
    if (n) memcpy(m, p, (size_t)n);
    m[n] = 0;
    return axs_lit(m, n);
}
static axs_str axs_cat(axs_str a, axs_str b) {
    char* m = (char*)malloc((size_t)(a.n + b.n) + 1);
    if (a.n) memcpy(m, a.p, (size_t)a.n);
    if (b.n) memcpy(m + a.n, b.p, (size_t)b.n);
    m[a.n + b.n] = 0;
    return axs_lit(m, a.n + b.n);
}
static axs_str axs_from_int(int64_t v) {
    char t[32];
    int n = snprintf(t, sizeof t, "%lld", (long long)v);
    return axs_new(t, n);
}
static bool axs_eq(axs_str a, axs_str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, (size_t)a.n) == 0); }
static axs_str axs_substring(axs_str s, int32_t start, int32_t n) {
    if (start < 0 || n < 0 || start + n > s.n) axs_fail("substring out of range", start, n);
    return axs_new(s.p + start, n);
}
static uint8_t axs_byteat(axs_str s, int32_t i) {
    if (i < 0 || i >= s.n) axs_fail("byteAt out of range", i, s.n);
    return (uint8_t)s.p[i];
}
static void axs_print_str(axs_str s) { fwrite(s.p, 1, (size_t)s.n, stdout); fputc('\n', stdout); }
static void axs_print_int(int64_t v) { printf("%lld\n", (long long)v); }
static void axs_print_bool(bool v) { puts(v ? "true" : "false"); }

static int64_t axs_check(int64_t len, int64_t i) {
    if (i < 0 || i >= len) axs_fail("index out of range", i, len);
    return i;
}
static void axs_list_add(axs_list* l, const void* v, int64_t size) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->data = (char*)realloc(l->data, (size_t)(l->cap * size));
    }
    memcpy(l->data + l->len * size, v, (size_t)size);
    l->len++;
}
static void* axs_list_pop(axs_list* l, int64_t size) {
    if (l->len == 0) axs_fail("pop on an empty list", 0, 0);
    l->len--;
    return l->data + l->len * size;
}
#define AXS_AT(T, l, i) (((T*)(l).data)[axs_check((l).len, (i))])

/* the standard functions axs programs can call */
static axs_str axs_readAll(axs_str path) {
    char* cp = (char*)malloc((size_t)path.n + 1);
    memcpy(cp, path.p, (size_t)path.n);
    cp[path.n] = 0;
    FILE* f = fopen(cp, "rb");
    if (!f) return axs_lit("", 0);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* m = (char*)malloc((size_t)n + 1);
    size_t got = fread(m, 1, (size_t)n, f);
    fclose(f);
    m[got] = 0;
    return axs_lit(m, (int64_t)got);
}
static bool axs_writeText(axs_str path, axs_str text) {
    char* cp = (char*)malloc((size_t)path.n + 1);
    memcpy(cp, path.p, (size_t)path.n);
    cp[path.n] = 0;
    FILE* f = fopen(cp, "wb");
    if (!f) return false;
    fwrite(text.p, 1, (size_t)text.n, f);
    fclose(f);
    return true;
}
static int32_t axs_argCount(void) { return axs_argc - 1; }
static axs_str axs_arg(int32_t i) {
    if (i < 0 || i + 1 >= axs_argc) return axs_lit("", 0);
    return axs_lit(axs_argv[i + 1], (int64_t)strlen(axs_argv[i + 1]));
}
#endif
