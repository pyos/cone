#include "romp.h"

enum romp_signo
{
    ROMP_SIGN_NONE   = 0,     // signature ::= typesign*
    ROMP_SIGN_ERROR  = 1,
    ROMP_SIGN_UINT   = 'u',   // typesign ::= 'u' {octets :: digit}
    ROMP_SIGN_INT    = 'i',   //            | 'd' {octets :: digit}
    ROMP_SIGN_DOUBLE = 'f',   //            | 'f'
    ROMP_SIGN_VEC    = 'v',   //            | 'v' {type :: typesign}
    ROMP_SIGN_STRUCT = '(',   //            | '(' {contents :: signature} ')';
    ROMP_SIGN_END    = ')',
};

struct romp_sign
{
    enum romp_signo sign;
    unsigned size;
    unsigned align;
    const char *contents;
    const char *next;
};

#define ALIGN(ptr, i) ((i) && (uintptr_t)(ptr) % (i) ? \
    ((ptr) = (__typeof__(ptr))((uintptr_t)(ptr) + (i) - (uintptr_t)(ptr) % (i))) : (ptr))

static struct romp_sign romp_sign(const char *sign, int accept_end) {
    while (*sign && *sign == ' ')
        sign++;
    struct romp_sign r = {.sign = *sign++};
    switch (r.sign) {
        case ROMP_SIGN_END:
        case ROMP_SIGN_NONE:
            if (accept_end)
                break;
            mun_error(romp_sign_syntax, "unexpected end of struct");
            goto fail;
        case ROMP_SIGN_INT:
        case ROMP_SIGN_UINT:
            r.size = (unsigned)*sign++ - '0';
            r.align = r.size == 1 ? _Alignof(uint8_t)
                    : r.size == 2 ? _Alignof(uint16_t)
                    : r.size == 4 ? _Alignof(uint32_t)
                    : r.size == 8 ? _Alignof(uint64_t) : 0;
            if (r.align)
                break;
            mun_error(romp_sign_syntax, "invalid int size '%c'", sign[-1]);
            goto fail;
        case ROMP_SIGN_DOUBLE:
            r.size = sizeof(double);
            r.align = _Alignof(double);
            break;
        case ROMP_SIGN_VEC:
            r.size = sizeof(struct mun_vec);
            r.align = _Alignof(struct mun_vec);
            r.contents = sign;
            struct romp_sign value = romp_sign(sign, 0);
            if (value.sign == ROMP_SIGN_ERROR)
                goto fail;
            sign = value.next;
            break;
        case ROMP_SIGN_STRUCT:
            r.contents = sign;
            while (1) {
                struct romp_sign field = romp_sign(sign, 1);
                sign = field.next;
                if (field.sign == ROMP_SIGN_ERROR)
                    goto fail;
                if (field.sign == ROMP_SIGN_END)
                    break;
                if (field.sign == ROMP_SIGN_NONE) {
                    mun_error(romp_sign_syntax, "mismatched (");
                    goto fail;
                }
                ALIGN(r.size, field.align);
                r.size += field.size;
                r.align = r.align > field.align ? r.align : field.align;
            }
            ALIGN(r.size, r.align);
            break;
        default:
            mun_error(romp_sign_syntax, "invalid sign '%c'", sign[-1]);
        fail:
            r.sign = ROMP_SIGN_ERROR;
            r.size = 1;
    }
    r.next = sign;
    return r;
}

static int romp_encode_uint(struct romp *out, uint64_t in, unsigned width) {
    uint8_t packed[] = { in >> 56, in >> 48, in >> 40, in >> 32, in >> 24, in >> 16, in >> 8, in };
    return mun_vec_extend(out, &packed[8 - width], width) MUN_RETHROW;
}

static int romp_decode_uint(struct romp *in, uint64_t *out, unsigned width) {
    if (in->size < width)
        return mun_error(romp, "need %u octets, have %u", width, in->size);
    *out = width == 1 ? romp_r1(in->data)
         : width == 2 ? romp_r2(in->data)
         : width == 4 ? romp_r4(in->data)
         : width == 8 ? romp_r8(in->data) : 0;
    mun_vec_erase(in, 0, width);
    return 0;
}

static int romp_encode_one(struct romp *out, struct romp_sign s, const void *in) {
    switch (s.sign) {
        case ROMP_SIGN_INT:
        case ROMP_SIGN_UINT:
        case ROMP_SIGN_DOUBLE:
            if (romp_encode_uint(out, s.size == 1 ? *(uint8_t *)in
                                    : s.size == 2 ? *(uint16_t*)in
                                    : s.size == 4 ? *(uint32_t*)in
                                    : s.size == 8 ? *(uint64_t*)in : 0, s.size) MUN_RETHROW)
                return -1;
            return 0;
        case ROMP_SIGN_VEC: {
            struct romp_sign q = romp_sign(s.contents, 0);
            const struct mun_vec *v = in;
            if (q.sign == ROMP_SIGN_ERROR || romp_encode_uint(out, v->size, 4) MUN_RETHROW)
                return -1;
            for (unsigned i = 0; i < v->size; i++)
                if (romp_encode_one(out, q, &v->data[q.size * i]) MUN_RETHROW)
                    return -1;
            return 0;
        }
        case ROMP_SIGN_STRUCT:
            if (romp_encode(out, s.contents, in) MUN_RETHROW)
                return -1;
        default:
            return 0;
    }
}

static int romp_decode_one(struct romp *in, struct romp_sign s, void *out) {
    uint64_t u = 0;
    switch (s.sign) {
        case ROMP_SIGN_INT:
        case ROMP_SIGN_UINT:
        case ROMP_SIGN_DOUBLE:
            if (romp_decode_uint(in, &u, s.size) MUN_RETHROW)
                return -1;
            if (s.size == 1) *(uint8_t *)out = u; else
            if (s.size == 2) *(uint16_t*)out = u; else
            if (s.size == 4) *(uint32_t*)out = u; else
            if (s.size == 8) *(uint64_t*)out = u;
            return 0;
        case ROMP_SIGN_VEC: {
            struct romp_sign q = romp_sign(s.contents, 0);
            struct mun_vec *v = out;
            if (q.sign == ROMP_SIGN_ERROR || romp_decode_uint(in, &u, 4) MUN_RETHROW)
                return -1;
            if (mun_vec_reserve_s(q.size, v, u) MUN_RETHROW)
                return -1;
            while (u--)
                if (romp_decode_one(in, q, &v->data[v->size++ * q.size]) MUN_RETHROW)
                    return mun_vec_fini(v), -1;
            return 0;
        }
        case ROMP_SIGN_STRUCT:
            if (romp_decode(in, s.contents, out) MUN_RETHROW)
                return -1;
        default:
            return 0;
    }
}

int romp_encode(struct romp *out, const char *sign, const void *in) {
    for (struct romp_sign s; (s = romp_sign(sign, 1)).size; in += s.size, sign = s.next)
        if (s.sign == ROMP_SIGN_ERROR || romp_encode_one(out, s, ALIGN(in, s.align)) MUN_RETHROW)
            return -1;
    return 0;
}

int romp_decode(struct romp *in, const char *sign, void *out) {
    for (struct romp_sign s; (s = romp_sign(sign, 1)).size; out += s.size, sign = s.next)
        if (s.sign == ROMP_SIGN_ERROR || romp_decode_one(in, s, ALIGN(out, s.align)) MUN_RETHROW)
            return -1;
    return 0;
}

struct romp_signinfo romp_signinfo(const char *sign) {
    struct romp_sign s = romp_sign(sign, 0);
    return (struct romp_signinfo){s.size, s.align};
}
