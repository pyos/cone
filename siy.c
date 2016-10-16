#include "siy.h"

enum siy_signo
{
    SIY_NONE   = 0,     // signature ::= typesign*
    SIY_ERROR  = 1,
    SIY_UINT   = 'u',   // typesign ::= 'u' {octets :: digit}
    SIY_INT    = 'i',   //            | 'd' {octets :: digit}
    SIY_DOUBLE = 'f',   //            | 'f'
    SIY_VEC    = 'v',   //            | 'v' {type :: typesign}
    SIY_STRUCT = '(',   //            | '(' {contents :: signature} ')';
    SIY_END    = ')',
};

struct siy_sign
{
    enum siy_signo sign;
    unsigned size;
    unsigned align;
    const char *contents;
    const char *next;
};

#define ALIGN(ptr, i) ((i) && (uintptr_t)(ptr) & ((i) - 1) ? \
    ((ptr) = (__typeof__(ptr))(((uintptr_t)(ptr) & ~((uintptr_t)(i) - 1)) + (i))) : (ptr))

static struct siy_sign siy_sign(const char *sign, int accept_end) {
    while (*sign && *sign == ' ')
        sign++;
    struct siy_sign r = {.sign = *sign++};
    switch (r.sign) {
        case SIY_END:
        case SIY_NONE:
            if (accept_end)
                break;
            mun_error(siy_sign_syntax, "unexpected end of struct");
            goto fail;
        case SIY_INT:
        case SIY_UINT:
            r.size = (unsigned)*sign++ - '0';
            r.align = r.size == 1 ? _Alignof(uint8_t)
                    : r.size == 2 ? _Alignof(uint16_t)
                    : r.size == 4 ? _Alignof(uint32_t)
                    : r.size == 8 ? _Alignof(uint64_t) : 0;
            if (r.align)
                break;
            mun_error(siy_sign_syntax, "invalid int size '%c'", sign[-1]);
            goto fail;
        case SIY_DOUBLE:
            r.size = sizeof(double);
            r.align = _Alignof(double);
            break;
        case SIY_VEC:
            r.size = sizeof(struct mun_vec);
            r.align = _Alignof(struct mun_vec);
            r.contents = sign;
            struct siy_sign value = siy_sign(sign, 0);
            if (value.sign == SIY_ERROR)
                goto fail;
            sign = value.next;
            break;
        case SIY_STRUCT:
            r.contents = sign;
            while (1) {
                struct siy_sign field = siy_sign(sign, 1);
                sign = field.next;
                if (field.sign == SIY_ERROR)
                    goto fail;
                if (field.sign == SIY_END)
                    break;
                if (field.sign == SIY_NONE) {
                    mun_error(siy_sign_syntax, "mismatched (");
                    goto fail;
                }
                ALIGN(r.size, field.align);
                r.size += field.size;
                r.align = r.align > field.align ? r.align : field.align;
            }
            ALIGN(r.size, r.align);
            break;
        default:
            mun_error(siy_sign_syntax, "invalid sign '%c'", sign[-1]);
        fail:
            r.sign = SIY_ERROR;
            r.size = 1;
    }
    r.next = sign;
    return r;
}

static int siy_encode_uint(struct siy *out, uint64_t in, unsigned width) {
    uint8_t packed[] = { in >> 56, in >> 48, in >> 40, in >> 32, in >> 24, in >> 16, in >> 8, in };
    return mun_vec_extend(out, &packed[8 - width], width) MUN_RETHROW;
}

static int siy_decode_uint(struct siy *in, uint64_t *out, unsigned width) {
    if (in->size < width)
        return mun_error(siy, "need %u octets, have %u", width, in->size);
    *out = width == 1 ? siy_r1(in->data)
         : width == 2 ? siy_r2(in->data)
         : width == 4 ? siy_r4(in->data)
         : width == 8 ? siy_r8(in->data) : 0;
    mun_vec_erase(in, 0, width);
    return 0;
}

static int siy_encode_one(struct siy *out, struct siy_sign s, const void *in) {
    switch (s.sign) {
        case SIY_INT:
        case SIY_UINT:
        case SIY_DOUBLE:
            if (siy_encode_uint(out, s.size == 1 ? *(uint8_t *)in
                                    : s.size == 2 ? *(uint16_t*)in
                                    : s.size == 4 ? *(uint32_t*)in
                                    : s.size == 8 ? *(uint64_t*)in : 0, s.size) MUN_RETHROW)
                return -1;
            return 0;
        case SIY_VEC: {
            struct siy_sign q = siy_sign(s.contents, 0);
            const struct mun_vec *v = in;
            if (q.sign == SIY_ERROR || siy_encode_uint(out, v->size, 4) MUN_RETHROW)
                return -1;
            for (unsigned i = 0; i < v->size; i++)
                if (siy_encode_one(out, q, &v->data[q.size * i]) MUN_RETHROW)
                    return -1;
            return 0;
        }
        case SIY_STRUCT:
            if (siy_encode(out, s.contents, in) MUN_RETHROW)
                return -1;
        default:
            return 0;
    }
}

static int siy_decode_one(struct siy *in, struct siy_sign s, void *out) {
    uint64_t u = 0;
    switch (s.sign) {
        case SIY_INT:
        case SIY_UINT:
        case SIY_DOUBLE:
            if (siy_decode_uint(in, &u, s.size) MUN_RETHROW)
                return -1;
            if (s.size == 1) *(uint8_t *)out = u; else
            if (s.size == 2) *(uint16_t*)out = u; else
            if (s.size == 4) *(uint32_t*)out = u; else
            if (s.size == 8) *(uint64_t*)out = u;
            return 0;
        case SIY_VEC: {
            struct siy_sign q = siy_sign(s.contents, 0);
            struct mun_vec *v = out;
            if (q.sign == SIY_ERROR || siy_decode_uint(in, &u, 4) MUN_RETHROW)
                return -1;
            if (mun_vec_reserve_s(q.size, v, u) MUN_RETHROW)
                return -1;
            while (u--)
                if (siy_decode_one(in, q, &v->data[v->size++ * q.size]) MUN_RETHROW)
                    return mun_vec_fini(v), -1;
            return 0;
        }
        case SIY_STRUCT:
            if (siy_decode(in, s.contents, out) MUN_RETHROW)
                return -1;
        default:
            return 0;
    }
}

int siy_encode(struct siy *out, const char *sign, const void *in) {
    for (struct siy_sign s; (s = siy_sign(sign, 1)).size; in += s.size, sign = s.next)
        if (s.sign == SIY_ERROR || siy_encode_one(out, s, ALIGN(in, s.align)) MUN_RETHROW)
            return -1;
    return 0;
}

int siy_decode(struct siy *in, const char *sign, void *out) {
    for (struct siy_sign s; (s = siy_sign(sign, 1)).size; out += s.size, sign = s.next)
        if (s.sign == SIY_ERROR || siy_decode_one(in, s, ALIGN(out, s.align)) MUN_RETHROW)
            return -1;
    return 0;
}

struct siy_signinfo siy_signinfo(const char *sign) {
    struct siy_sign s = siy_sign(sign, 0);
    return (struct siy_signinfo){s.size, s.align};
}
