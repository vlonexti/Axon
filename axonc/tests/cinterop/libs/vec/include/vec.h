/* vec.h - a tiny header-only C library used to test Axon's C interop. */
#ifndef VEC_H
#define VEC_H
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

typedef struct Vec2 { float x; float y; } Vec2;
typedef struct Color { unsigned char r, g, b, a; } Color;

#define ORIGIN ((Vec2){ 0.0f, 0.0f })
#define RED ((Color){ 230, 41, 55, 255 })
#define KEY_SPACE 32

static inline Vec2 vec_add(Vec2 a, Vec2 b) { Vec2 r = { a.x + b.x, a.y + b.y }; return r; }
static inline float vec_dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
static inline void vec_scale(Vec2* v, float k) { v->x *= k; v->y *= k; }
static inline int greet(const char* name) { printf("C says hello to %s\n", name); return (int)strlen(name); }
static inline const char* color_name(Color c) { return c.r > 200 ? "reddish" : "other"; }
static inline bool is_key_down(int key) { return key == KEY_SPACE; }
#endif
