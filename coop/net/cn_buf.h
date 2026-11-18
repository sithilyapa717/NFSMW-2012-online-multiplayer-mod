/* Bounds-checked little-endian writer and reader. Errors are sticky. */
#ifndef CN_BUF_H
#define CN_BUF_H

#include "cn_common.h"

typedef struct {
    uint8_t *p;
    size_t cap, len;
    int err;
} cn_w;

typedef struct {
    const uint8_t *p;
    size_t len, pos;
    int err;
} cn_r;

static inline void cn_w_init(cn_w *w, uint8_t *p, size_t cap)
{
    w->p = p;
    w->cap = cap;
    w->len = 0;
    w->err = 0;
}

static inline void cn_w_bytes(cn_w *w, const void *d, size_t n)
{
    if (w->err || w->len + n > w->cap) {
        w->err = 1;
        return;
    }
    if (n)
        memcpy(w->p + w->len, d, n);
    w->len += n;
}

static inline void cn_w_u8(cn_w *w, uint8_t v) { cn_w_bytes(w, &v, 1); }

static inline void cn_w_u16(cn_w *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    cn_w_bytes(w, b, 2);
}

static inline void cn_w_u32(cn_w *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    cn_w_bytes(w, b, 4);
}

static inline void cn_w_f32(cn_w *w, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    cn_w_u32(w, u);
}

static inline void cn_r_init(cn_r *r, const uint8_t *p, size_t len)
{
    r->p = p;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

static inline int cn_r_need(cn_r *r, size_t n)
{
    if (r->err || r->pos + n > r->len) {
        r->err = 1;
        return 0;
    }
    return 1;
}

static inline void cn_r_bytes(cn_r *r, void *d, size_t n)
{
    if (!cn_r_need(r, n)) {
        memset(d, 0, n);
        return;
    }
    memcpy(d, r->p + r->pos, n);
    r->pos += n;
}

static inline uint8_t cn_r_u8(cn_r *r)
{
    uint8_t v = 0;
    cn_r_bytes(r, &v, 1);
    return v;
}

static inline uint16_t cn_r_u16(cn_r *r)
{
    uint8_t b[2];
    cn_r_bytes(r, b, 2);
    return (uint16_t)(b[0] | (b[1] << 8));
}

static inline uint32_t cn_r_u32(cn_r *r)
{
    uint8_t b[4];
    cn_r_bytes(r, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline float cn_r_f32(cn_r *r)
{
    uint32_t u = cn_r_u32(r);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

#endif
