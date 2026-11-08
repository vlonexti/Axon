// runtime.cpp - the C runtime pasted at the top of every generated program.
// Split into pieces because MSVC limits the length of a single string literal.
#include "runtime.h"

const char* const AXON_RT_CORE_1 = R"AXRT(
/* ---- Axon runtime ---- */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>

#if defined(_MSC_VER)
#define AX_INLINE static __inline
#else
#define AX_INLINE static inline
#endif

typedef uint32_t ax_char;
typedef struct ax_string { const char* ptr; int64_t len; int64_t cap; } ax_string;

static int ax_test_mode = 0;
static jmp_buf ax_test_jmp;
static char ax_test_msg[1024];
static int ax_argc = 0;
static char** ax_argv = NULL;

static void ax_panic_at(const char* msg, const char* loc) {
    fflush(stdout);
    if (ax_test_mode) {
        snprintf(ax_test_msg, sizeof ax_test_msg, "%s (at %s)", msg, loc);
        longjmp(ax_test_jmp, 1);
    }
    fprintf(stderr, "panic: %s\n  at %s\n", msg, loc);
    fflush(stderr);
    exit(101);
}

static void ax_panicf(const char* loc, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ax_panic_at(buf, loc);
}

/* Every heap allocation goes through these, so debug builds can count live blocks:
   run a program with AXON_LEAKCHECK=1 (or `axon run --leak-check`) to see if anything leaked. */
static int64_t ax_live_allocs = 0;
static void* ax_alloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "panic: out of memory\n"); exit(101); }
    ax_live_allocs++;
    return p;
}
static void* ax_calloc(size_t n, size_t sz) {
    void* p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) { fprintf(stderr, "panic: out of memory\n"); exit(101); }
    ax_live_allocs++;
    return p;
}
static void* ax_realloc(void* old, size_t n) {
    void* p = realloc(old, n ? n : 1);
    if (!p) { fprintf(stderr, "panic: out of memory\n"); exit(101); }
    if (!old) ax_live_allocs++;
    return p;
}
static void ax_free(void* p) {
    if (p) { ax_live_allocs--; free(p); }
}
static void ax_leak_report(void) {
#ifndef AXON_RELEASE
    const char* e = getenv("AXON_LEAKCHECK");
    if (e && *e && *e != '0') {
        fflush(stdout);
        if (ax_live_allocs == 0) fprintf(stderr, "leak check: no leaks (every allocation was freed)\n");
        else fprintf(stderr, "leak check: %lld allocation(s) were never freed\n", (long long)ax_live_allocs);
    }
#endif
}

/* hashing for Map keys */
AX_INLINE uint64_t ax_hash_u64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
/* reverse a run of `len` items of `sz` bytes in place */
static void ax_reverse(void* data, int64_t len, size_t sz) {
    char* d = (char*)data;
    char tmp[256];
    for (int64_t i = 0, j = len - 1; i < j; i++, j--) {
        char* a = d + (size_t)i * sz;
        char* b = d + (size_t)j * sz;
        for (size_t o = 0; o < sz; o += sizeof tmp) {
            size_t n = sz - o < sizeof tmp ? sz - o : sizeof tmp;
            memcpy(tmp, a + o, n); memcpy(a + o, b + o, n); memcpy(b + o, tmp, n);
        }
    }
}

/* ---- checked arithmetic (checks removed in release builds) ---- */
AX_INLINE int32_t ax_add_i32(int32_t a, int32_t b, const char* loc) {
#ifndef AXON_RELEASE
    int64_t r = (int64_t)a + (int64_t)b;
    if (r > INT32_MAX || r < INT32_MIN) ax_panicf(loc, "integer overflow: %d + %d doesn't fit in an int", a, b);
    return (int32_t)r;
#else
    (void)loc; return (int32_t)((uint32_t)a + (uint32_t)b);
#endif
}
AX_INLINE int32_t ax_sub_i32(int32_t a, int32_t b, const char* loc) {
#ifndef AXON_RELEASE
    int64_t r = (int64_t)a - (int64_t)b;
    if (r > INT32_MAX || r < INT32_MIN) ax_panicf(loc, "integer overflow: %d - %d doesn't fit in an int", a, b);
    return (int32_t)r;
#else
    (void)loc; return (int32_t)((uint32_t)a - (uint32_t)b);
#endif
}
AX_INLINE int32_t ax_mul_i32(int32_t a, int32_t b, const char* loc) {
#ifndef AXON_RELEASE
    int64_t r = (int64_t)a * (int64_t)b;
    if (r > INT32_MAX || r < INT32_MIN) ax_panicf(loc, "integer overflow: %d * %d doesn't fit in an int", a, b);
    return (int32_t)r;
#else
    (void)loc; return (int32_t)((uint32_t)a * (uint32_t)b);
#endif
}
AX_INLINE int32_t ax_div_i32(int32_t a, int32_t b, const char* loc) {
    if (b == 0) ax_panic_at("division by zero", loc);
    if (a == INT32_MIN && b == -1) ax_panic_at("integer overflow in division", loc);
    return a / b;
}
AX_INLINE int32_t ax_mod_i32(int32_t a, int32_t b, const char* loc) {
    if (b == 0) ax_panic_at("division by zero (in %)", loc);
    if (b == -1) return 0;
    return a % b;
}
AX_INLINE int32_t ax_neg_i32(int32_t a, const char* loc) {
#ifndef AXON_RELEASE
    if (a == INT32_MIN) ax_panic_at("integer overflow in negation", loc);
#endif
    (void)loc; return (int32_t)(0u - (uint32_t)a);
}
AX_INLINE int64_t ax_add_i64(int64_t a, int64_t b, const char* loc) {
#ifndef AXON_RELEASE
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) ax_panicf(loc, "integer overflow: %lld + %lld doesn't fit in a long", (long long)a, (long long)b);
#endif
    (void)loc; return (int64_t)((uint64_t)a + (uint64_t)b);
}
AX_INLINE int64_t ax_sub_i64(int64_t a, int64_t b, const char* loc) {
#ifndef AXON_RELEASE
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) ax_panicf(loc, "integer overflow: %lld - %lld doesn't fit in a long", (long long)a, (long long)b);
#endif
    (void)loc; return (int64_t)((uint64_t)a - (uint64_t)b);
}
AX_INLINE int64_t ax_mul_i64(int64_t a, int64_t b, const char* loc) {
#ifndef AXON_RELEASE
    if (a != 0 && b != 0) {
        int bad = 0;
        if (a > 0) { if (b > 0) bad = a > INT64_MAX / b; else bad = b < INT64_MIN / a; }
        else { if (b > 0) bad = a < INT64_MIN / b; else bad = a < INT64_MAX / b; }
        if (bad) ax_panicf(loc, "integer overflow: %lld * %lld doesn't fit in a long", (long long)a, (long long)b);
    }
#endif
    (void)loc; return (int64_t)((uint64_t)a * (uint64_t)b);
}
AX_INLINE int64_t ax_div_i64(int64_t a, int64_t b, const char* loc) {
    if (b == 0) ax_panic_at("division by zero", loc);
    if (a == INT64_MIN && b == -1) ax_panic_at("integer overflow in division", loc);
    return a / b;
}
AX_INLINE int64_t ax_mod_i64(int64_t a, int64_t b, const char* loc) {
    if (b == 0) ax_panic_at("division by zero (in %)", loc);
    if (b == -1) return 0;
    return a % b;
}
AX_INLINE int64_t ax_neg_i64(int64_t a, const char* loc) {
#ifndef AXON_RELEASE
    if (a == INT64_MIN) ax_panic_at("integer overflow in negation", loc);
#endif
    (void)loc; return (int64_t)(0ull - (uint64_t)a);
}

#define AX_INCDEC(NAME, T, CHECK)                                                        \
    AX_INLINE T ax_postinc_##NAME(T* p, const char* loc) { T old = *p; CHECK(old, 1); *p = (T)(old + 1); return old; } \
    AX_INLINE T ax_postdec_##NAME(T* p, const char* loc) { T old = *p; CHECK(old, -1); *p = (T)(old - 1); return old; } \
    AX_INLINE T ax_preinc_##NAME(T* p, const char* loc) { CHECK(*p, 1); *p = (T)(*p + 1); return *p; } \
    AX_INLINE T ax_predec_##NAME(T* p, const char* loc) { CHECK(*p, -1); *p = (T)(*p - 1); return *p; }

#ifndef AXON_RELEASE
#define AX_CHK_I32(v, d) if ((d > 0 && v == INT32_MAX) || (d < 0 && v == INT32_MIN)) ax_panic_at("integer overflow in ++/--", loc)
#define AX_CHK_I64(v, d) if ((d > 0 && v == INT64_MAX) || (d < 0 && v == INT64_MIN)) ax_panic_at("integer overflow in ++/--", loc)
#define AX_CHK_U8(v, d) if ((d > 0 && v == 255) || (d < 0 && v == 0)) ax_panic_at("byte overflow in ++/--", loc)
#else
#define AX_CHK_I32(v, d) (void)loc
#define AX_CHK_I64(v, d) (void)loc
#define AX_CHK_U8(v, d) (void)loc
#endif
#define AX_CHK_NONE(v, d) (void)loc
AX_INCDEC(i32, int32_t, AX_CHK_I32)
AX_INCDEC(i64, int64_t, AX_CHK_I64)
AX_INCDEC(u8, uint8_t, AX_CHK_U8)
AX_INCDEC(u32, uint32_t, AX_CHK_NONE)
AX_INCDEC(f32, float, AX_CHK_NONE)
AX_INCDEC(f64, double, AX_CHK_NONE)

AX_INLINE void ax_bounds(int64_t i, int64_t len, const char* loc) {
#ifndef AXON_RELEASE
    if (i < 0 || i >= len) ax_panicf(loc, "index %lld is out of range (length is %lld)", (long long)i, (long long)len);
#else
    (void)i; (void)len; (void)loc;
#endif
}
)AXRT";

const char* const AXON_RT_CORE_2 = R"AXRT(
/* ---- strings ---- */
AX_INLINE ax_string ax_str_lit(const char* s, int64_t n) { ax_string r; r.ptr = s; r.len = n; r.cap = 0; return r; }
AX_INLINE void ax_drop_string(ax_string* s) {
    if (s->cap > 0) ax_free((void*)s->ptr);
    s->ptr = NULL; s->len = 0; s->cap = 0;
}
static ax_string ax_str_new(const char* p, int64_t n) {
    char* m = (char*)ax_alloc((size_t)n + 1);
    if (n) memcpy(m, p, (size_t)n);
    m[n] = 0;
    ax_string r; r.ptr = m; r.len = n; r.cap = n + 1;
    return r;
}
AX_INLINE ax_string ax_clone_string(ax_string* s) { return s->cap > 0 ? ax_str_new(s->ptr, s->len) : *s; }
AX_INLINE ax_string ax_move_string(ax_string* s) { ax_string r = *s; s->ptr = NULL; s->len = 0; s->cap = 0; return r; }
AX_INLINE const char* ax_cstr(ax_string s) { return s.ptr ? s.ptr : ""; }
static ax_string ax_str_from_c(const char* p) { return p ? ax_str_lit(p, (int64_t)strlen(p)) : ax_str_lit("", 0); }
AX_INLINE bool ax_str_eq(ax_string a, ax_string b) { return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, (size_t)a.len) == 0); }

typedef struct ax_sb { char* p; int64_t len; int64_t cap; } ax_sb;
AX_INLINE ax_sb ax_sb_new(void) { ax_sb b; b.p = NULL; b.len = 0; b.cap = 0; return b; }
static void ax_sb_raw(ax_sb* b, const char* s, int64_t n) {
    if (b->len + n + 1 > b->cap) {
        int64_t nc = b->cap ? b->cap * 2 : 32;
        while (nc < b->len + n + 1) nc *= 2;
        b->p = (char*)ax_realloc(b->p, (size_t)nc);
        b->cap = nc;
    }
    if (n) memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
}
AX_INLINE void ax_sb_str(ax_sb* b, ax_string s) { ax_sb_raw(b, s.ptr, s.len); }
static void ax_sb_i64(ax_sb* b, int64_t v) { char t[32]; int n = snprintf(t, sizeof t, "%lld", (long long)v); ax_sb_raw(b, t, n); }
/* Shortest text that reads back as the same number, written like C#: 450, 0.1, 1E+20. */
static void ax_fmt_real(char* t, size_t cap, double v, int maxDigits, int isFloat) {
    if (v != v) { snprintf(t, cap, "NaN"); return; }
    if (v > 1.7976931348623157e308) { snprintf(t, cap, "Infinity"); return; }
    if (v < -1.7976931348623157e308) { snprintf(t, cap, "-Infinity"); return; }
    if (v == 0) { snprintf(t, cap, "0"); return; }
    char e[64];
    int digits = 1;
    for (; digits <= maxDigits; digits++) {
        snprintf(e, sizeof e, "%.*e", digits - 1, v);
        double back = strtod(e, NULL);
        if (isFloat ? (float)back == (float)v : back == v) break;
    }
    if (digits > maxDigits) digits = maxDigits;
    int exp = atoi(strchr(e, 'e') + 1);
    if (exp >= -5 && exp < 15) {
        int decimals = digits - 1 - exp;
        snprintf(t, cap, "%.*f", decimals > 0 ? decimals : 0, v);
    } else {
        /* d.dddE+XX */
        char* ep = strchr(e, 'e');
        *ep = 0;
        snprintf(t, cap, "%sE%c%02d", e, exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
    }
}
static void ax_fmt_f64(char* t, size_t cap, double v) { ax_fmt_real(t, cap, v, 17, 0); }
static void ax_fmt_f32(char* t, size_t cap, float v) { ax_fmt_real(t, cap, (double)v, 9, 1); }
static void ax_sb_f64(ax_sb* b, double v) { char t[64]; ax_fmt_f64(t, sizeof t, v); ax_sb_raw(b, t, (int64_t)strlen(t)); }
static void ax_sb_f32(ax_sb* b, float v) { char t[64]; ax_fmt_f32(t, sizeof t, v); ax_sb_raw(b, t, (int64_t)strlen(t)); }
AX_INLINE void ax_sb_bool(ax_sb* b, bool v) { if (v) ax_sb_raw(b, "true", 4); else ax_sb_raw(b, "false", 5); }
static int ax_utf8(char* o, ax_char c) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); return 4;
}
AX_INLINE void ax_sb_char(ax_sb* b, ax_char c) { char t[4]; int n = ax_utf8(t, c); ax_sb_raw(b, t, n); }
static ax_string ax_sb_done(ax_sb* b) {
    ax_sb_raw(b, "", 0);
    if (!b->p) return ax_str_new("", 0);
    b->p[b->len] = 0;
    ax_string r; r.ptr = b->p; r.len = b->len; r.cap = b->cap;
    return r;
}

/* ---- printing ---- */
static void ax_write_str(ax_string s) { if (s.len) fwrite(s.ptr, 1, (size_t)s.len, stdout); }
static void ax_write_i64(int64_t v) { printf("%lld", (long long)v); }
static void ax_write_f64(double v) { char t[64]; ax_fmt_f64(t, sizeof t, v); fputs(t, stdout); }
static void ax_write_f32(float v) { char t[64]; ax_fmt_f32(t, sizeof t, v); fputs(t, stdout); }
static void ax_write_bool(bool v) { fputs(v ? "true" : "false", stdout); }
static void ax_write_char(ax_char c) { char t[5]; int n = ax_utf8(t, c); fwrite(t, 1, (size_t)n, stdout); }
static void ax_print_newline(void) { fputc('\n', stdout); }
static void ax_print_str(ax_string s) { ax_write_str(s); fputc('\n', stdout); }
static void ax_print_i64(int64_t v) { ax_write_i64(v); fputc('\n', stdout); }
static void ax_print_f64(double v) { ax_write_f64(v); fputc('\n', stdout); }
static void ax_print_f32(float v) { ax_write_f32(v); fputc('\n', stdout); }
static void ax_print_bool(bool v) { ax_write_bool(v); fputc('\n', stdout); }
static void ax_print_char(ax_char c) { ax_write_char(c); fputc('\n', stdout); }

static void ax_assert(bool c, const char* loc) { if (!c) ax_panic_at("assertion failed", loc); }
static void ax_assert_msg(bool c, ax_string msg, const char* loc) {
    if (!c) { char buf[600]; snprintf(buf, sizeof buf, "assertion failed: %.*s", (int)(msg.len > 500 ? 500 : msg.len), ax_cstr(msg)); ax_panic_at(buf, loc); }
}
static void ax_panic_str(ax_string msg, const char* loc) {
    char buf[600]; snprintf(buf, sizeof buf, "%.*s", (int)(msg.len > 590 ? 590 : msg.len), ax_cstr(msg)); ax_panic_at(buf, loc);
}

/* ---- string methods ---- */
static ax_string ax_str_substring(ax_string s, int32_t start, int32_t n, const char* loc) {
    if (start < 0 || n < 0 || (int64_t)start + n > s.len)
        ax_panicf(loc, "substring(%d, %d) is out of range for a string of length %lld", start, n, (long long)s.len);
    return ax_str_new(s.ptr + start, n);
}
static int32_t ax_str_indexof(ax_string s, ax_string sub) {
    if (sub.len == 0) return 0;
    for (int64_t i = 0; i + sub.len <= s.len; i++)
        if (memcmp(s.ptr + i, sub.ptr, (size_t)sub.len) == 0) return (int32_t)i;
    return -1;
}
AX_INLINE bool ax_str_contains(ax_string s, ax_string sub) { return ax_str_indexof(s, sub) >= 0; }
AX_INLINE bool ax_str_startswith(ax_string s, ax_string p) { return p.len <= s.len && (p.len == 0 || memcmp(s.ptr, p.ptr, (size_t)p.len) == 0); }
AX_INLINE bool ax_str_endswith(ax_string s, ax_string p) { return p.len <= s.len && (p.len == 0 || memcmp(s.ptr + s.len - p.len, p.ptr, (size_t)p.len) == 0); }
static ax_string ax_str_trim(ax_string s) {
    int64_t a = 0, b = s.len;
    while (a < b && isspace((unsigned char)s.ptr[a])) a++;
    while (b > a && isspace((unsigned char)s.ptr[b - 1])) b--;
    return ax_str_new(s.ptr + a, b - a);
}
static ax_string ax_str_toupper(ax_string s) {
    ax_string r = ax_str_new(s.ptr, s.len);
    for (int64_t i = 0; i < r.len; i++) ((char*)r.ptr)[i] = (char)toupper((unsigned char)r.ptr[i]);
    return r;
}
static ax_string ax_str_tolower(ax_string s) {
    ax_string r = ax_str_new(s.ptr, s.len);
    for (int64_t i = 0; i < r.len; i++) ((char*)r.ptr)[i] = (char)tolower((unsigned char)r.ptr[i]);
    return r;
}
static ax_string ax_str_replace(ax_string s, ax_string from, ax_string to) {
    if (from.len == 0) return ax_str_new(s.ptr, s.len);
    ax_sb b = ax_sb_new();
    int64_t i = 0;
    while (i < s.len) {
        if (i + from.len <= s.len && memcmp(s.ptr + i, from.ptr, (size_t)from.len) == 0) { ax_sb_str(&b, to); i += from.len; }
        else { ax_sb_raw(&b, s.ptr + i, 1); i++; }
    }
    return ax_sb_done(&b);
}
static int32_t ax_str_lastindexof(ax_string s, ax_string sub) {
    if (sub.len > s.len) return -1;
    for (int64_t i = s.len - sub.len; i >= 0; i--)
        if (sub.len == 0 || memcmp(s.ptr + i, sub.ptr, (size_t)sub.len) == 0) return (int32_t)i;
    return -1;
}
static ax_string ax_str_pad(ax_string s, int32_t width, bool left) {
    ax_sb b = ax_sb_new();
    int64_t pad = width > s.len ? width - s.len : 0;
    if (!left) ax_sb_str(&b, s);
    for (int64_t i = 0; i < pad; i++) ax_sb_raw(&b, " ", 1);
    if (left) ax_sb_str(&b, s);
    return ax_sb_done(&b);
}
static ax_string ax_str_repeat(ax_string s, int32_t n, const char* loc) {
    if (n < 0) ax_panicf(loc, "repeat(%d): the count can't be negative", n);
    ax_sb b = ax_sb_new();
    for (int32_t i = 0; i < n; i++) ax_sb_str(&b, s);
    return ax_sb_done(&b);
}
AX_INLINE uint8_t ax_str_byteat(ax_string s, int32_t i, const char* loc) {
    if (i < 0 || i >= s.len) ax_panicf(loc, "byteAt(%d) is out of range for a string of length %lld", i, (long long)s.len);
    return (uint8_t)s.ptr[i];
}

#ifdef _WIN32
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int);
#endif
static void ax_rt_init(int argc, char** argv) {
    ax_argc = argc;
    ax_argv = argv;
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
}

typedef void (*ax_test_fn)(void);
static void ax_run_test(const char* name, ax_test_fn f, int* passed, int* failed) {
    printf("test %s ... ", name);
    fflush(stdout);
    ax_test_mode = 1;
    if (setjmp(ax_test_jmp) == 0) {
        f();
        printf("ok\n");
        (*passed)++;
    } else {
        printf("FAILED\n    %s\n", ax_test_msg);
        (*failed)++;
    }
    ax_test_mode = 0;
    fflush(stdout);
}
/* ---- end of core runtime ---- */
)AXRT";

// Runtime pieces that use generated types (ax_arr_string, ax_opt_int, ...). Emitted after type definitions.
const char* const AXON_RT_AFTER_TYPES = R"AXRT(
/* ---- runtime functions that use generated types ---- */
/* UTF-8 text -> one char per code point */
static ax_arr_char ax_str_chars(ax_string s) {
    ax_arr_char r; r.len = 0;
    r.data = s.len ? (ax_char*)ax_alloc((size_t)s.len * sizeof(ax_char)) : NULL;
    for (int64_t i = 0; i < s.len;) {
        unsigned char c = (unsigned char)s.ptr[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        ax_char v = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < n && i + k < s.len; k++) v = (v << 6) | ((unsigned char)s.ptr[i + k] & 0x3F);
        r.data[r.len++] = v;
        i += n;
    }
    return r;
}
/* string? <-> a C string that may be NULL */
static const char* ax_opt_cstr(ax_opt_string s) { return s.has ? ax_cstr(s.val) : NULL; }
static ax_opt_string ax_opt_from_c(const char* p) {
    ax_opt_string r; r.has = p != NULL; r.val = ax_str_from_c(p);
    return r;
}
static ax_arr_string ax_str_split(ax_string s, ax_string sep) {
    ax_arr_string r; r.data = NULL; r.len = 0;
    int64_t cap = 0;
#define AX_SPLIT_PUSH(P, N) do { \
        if (r.len == cap) { cap = cap ? cap * 2 : 8; r.data = (ax_string*)ax_realloc(r.data, (size_t)cap * sizeof(ax_string)); } \
        r.data[r.len++] = ax_str_new((P), (N)); } while (0)
    if (sep.len == 0) {
        for (int64_t i = 0; i < s.len; i++) AX_SPLIT_PUSH(s.ptr + i, 1);
        return r;
    }
    int64_t start = 0, i = 0;
    while (i + sep.len <= s.len) {
        if (memcmp(s.ptr + i, sep.ptr, (size_t)sep.len) == 0) {
            AX_SPLIT_PUSH(s.ptr + start, i - start);
            i += sep.len;
            start = i;
        } else i++;
    }
    AX_SPLIT_PUSH(s.ptr + start, s.len - start);
#undef AX_SPLIT_PUSH
    return r;
}
static ax_opt_int ax_str_toint(ax_string s) {
    ax_opt_int r; r.has = false; r.val = 0;
    if (s.len == 0 || s.len > 30) return r;
    char buf[32]; memcpy(buf, s.ptr, (size_t)s.len); buf[s.len] = 0;
    char* end; errno = 0;
    long long v = strtoll(buf, &end, 10);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != 0 || end == buf || v > INT32_MAX || v < INT32_MIN) return r;
    r.has = true; r.val = (int32_t)v;
    return r;
}
static ax_opt_double ax_str_todouble(ax_string s) {
    ax_opt_double r; r.has = false; r.val = 0;
    if (s.len == 0 || s.len > 60) return r;
    char buf[64]; memcpy(buf, s.ptr, (size_t)s.len); buf[s.len] = 0;
    char* end;
    double v = strtod(buf, &end);
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end != 0 || end == buf) return r;
    r.has = true; r.val = v;
    return r;
}

/* std.io */
static ax_opt_string axrt_readLine(void) {
    ax_opt_string r; r.has = false; r.val = ax_str_lit("", 0);
    fflush(stdout);
    ax_sb b = ax_sb_new();
    int c, any = 0;
    while ((c = fgetc(stdin)) != EOF) {
        any = 1;
        if (c == '\n') break;
        char ch = (char)c;
        ax_sb_raw(&b, &ch, 1);
    }
    if (!any) { ax_free(b.p); return r; }
    if (b.len > 0 && b.p[b.len - 1] == '\r') b.len--;
    r.has = true; r.val = ax_sb_done(&b);
    return r;
}
static ax_res_string_string axrt_readFile(ax_string path) {
    ax_res_string_string r; memset(&r, 0, sizeof r);
    FILE* f = fopen(ax_cstr(path), "rb");
    if (!f) {
        ax_sb b = ax_sb_new(); ax_sb_raw(&b, "can't open ", 11); ax_sb_str(&b, path);
        r.tag = 1; r.u.err = ax_sb_done(&b);
        return r;
    }
    ax_sb b = ax_sb_new();
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) ax_sb_raw(&b, buf, (int64_t)n);
    fclose(f);
    r.tag = 0; r.u.ok = ax_sb_done(&b);
    return r;
}
static bool axrt_writeFile(ax_string path, ax_string text) {
    FILE* f = fopen(ax_cstr(path), "wb");
    if (!f) return false;
    size_t n = text.len ? fwrite(text.ptr, 1, (size_t)text.len, f) : 0;
    fclose(f);
    return (int64_t)n == text.len;
}
static bool axrt_appendFile(ax_string path, ax_string text) {
    FILE* f = fopen(ax_cstr(path), "ab");
    if (!f) return false;
    size_t n = text.len ? fwrite(text.ptr, 1, (size_t)text.len, f) : 0;
    fclose(f);
    return (int64_t)n == text.len;
}
static bool axrt_fileExists(ax_string path) {
    FILE* f = fopen(ax_cstr(path), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
static bool axrt_deleteFile(ax_string path) { return remove(ax_cstr(path)) == 0; }

/* std.os */
static int32_t axrt_run(ax_string command) {
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    /* cmd.exe strips one pair of outer quotes, so wrap the whole command */
    ax_sb b = ax_sb_new();
    ax_sb_raw(&b, "\"", 1);
    ax_sb_str(&b, command);
    ax_sb_raw(&b, "\"", 1);
    ax_string full = ax_sb_done(&b);
    int rc = system(ax_cstr(full));
    ax_drop_string(&full);
    return rc;
#else
    int rc = system(ax_cstr(command));
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
#endif
}
static ax_opt_string axrt_env(ax_string name) {
    ax_opt_string r; r.has = false; r.val = ax_str_lit("", 0);
    const char* v = getenv(ax_cstr(name));
    if (v) { r.has = true; r.val = ax_str_new(v, (int64_t)strlen(v)); }
    return r;
}
static void axrt_exit(int32_t code) { fflush(stdout); exit(code); }

static ax_string axrt_readAll(ax_string path) {
    ax_res_string_string r = axrt_readFile(path);
    if (r.tag == 0) return r.u.ok;
    ax_drop_string(&r.u.err);
    return ax_str_lit("", 0);
}
static int32_t axrt_argCount(void) { return ax_argc > 1 ? ax_argc - 1 : 0; }
static ax_string axrt_arg(int32_t i) {
    if (i < 0 || i + 1 >= ax_argc) return ax_str_lit("", 0);
    return ax_str_new(ax_argv[i + 1], (int64_t)strlen(ax_argv[i + 1]));
}
static ax_arr_string axrt_args(void) {
    ax_arr_string r; r.len = ax_argc > 1 ? ax_argc - 1 : 0;
    r.data = r.len ? (ax_string*)ax_alloc((size_t)r.len * sizeof(ax_string)) : NULL;
    for (int64_t i = 0; i < r.len; i++) r.data[i] = ax_str_new(ax_argv[i + 1], (int64_t)strlen(ax_argv[i + 1]));
    return r;
}

/* std.random */
static uint64_t ax_rng_state = 0;
static uint64_t ax_rng_next(void) {
    if (ax_rng_state == 0) ax_rng_state = (uint64_t)time(NULL) * 2654435761u ^ 0x9E3779B97F4A7C15ull;
    uint64_t x = ax_rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    ax_rng_state = x;
    return x;
}
static void axrt_randomSeed(int64_t seed) { ax_rng_state = (uint64_t)seed * 6364136223846793005ull + 1442695040888963407ull; if (!ax_rng_state) ax_rng_state = 1; }
static int32_t axrt_randomInt(int32_t lo, int32_t hi) {
    if (hi < lo) { int32_t t = lo; lo = hi; hi = t; }
    uint64_t span = (uint64_t)((int64_t)hi - (int64_t)lo) + 1;
    return (int32_t)((int64_t)lo + (int64_t)(ax_rng_next() % span));
}
static double axrt_randomDouble(void) { return (double)(ax_rng_next() >> 11) / 9007199254740992.0; }

/* std.time */
#ifdef _WIN32
__declspec(dllimport) void __stdcall Sleep(unsigned long);
#else
#include <unistd.h>
#endif
static int64_t axrt_timeMillis(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void axrt_sleepMillis(int32_t ms) {
#ifdef _WIN32
    Sleep((unsigned long)(ms < 0 ? 0 : ms));
#else
    usleep((useconds_t)(ms < 0 ? 0 : ms) * 1000);
#endif
}
)AXRT";
