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

enum romp_parse_flags
{
    ROMP_ACCEPT_NONE = 0x1,
    ROMP_ACCEPT_END  = 0x2,
};

struct romp_sign
{
    enum romp_signo sign;
    unsigned size;
    unsigned align;
};

struct romp_nested_vec mun_vec(struct mun_vec);

// Decode a single element from a signature, advancing the pointer to the next one.
// If the returned element is a vector, the next element is the type of its contents;
// if it's a structure, call this function again until it returns a ROMP_SIGN_END to
// get element types.
//
// Errors: `romp_sign_syntax` if the signature is invalid.
//
static struct romp_sign romp_sign(const char **sign, enum romp_parse_flags flags) {
    const char *s = *sign;
    while (*s && *s == ' ')
        s++;
    struct romp_sign r = {};
    switch ((r.sign = *s++)) {
        case ROMP_SIGN_NONE:
            if (!(flags & ROMP_ACCEPT_NONE)) {
                r.sign = ROMP_SIGN_ERROR;
                mun_error(romp_sign_syntax, "unexpected end of signature");
            }
            break;
        case ROMP_SIGN_END:
            if (!(flags & ROMP_ACCEPT_END)) {
                r.sign = ROMP_SIGN_ERROR;
                mun_error(romp_sign_syntax, "unexpected end of struct");
            }
            break;
        case ROMP_SIGN_DOUBLE:
            r.align = _Alignof(double);
            break;
        case ROMP_SIGN_VEC:
            r.size = sizeof(struct mun_vec);
            r.align = _Alignof(struct mun_vec);
            break;
        case ROMP_SIGN_INT:
        case ROMP_SIGN_UINT:
            r.size = (unsigned)*s++ - '0';
            r.align = r.size == 1 ? _Alignof(uint8_t)
                    : r.size == 2 ? _Alignof(uint16_t)
                    : r.size == 4 ? _Alignof(uint32_t)
                    : r.size == 8 ? _Alignof(uint64_t) : 0;
            if (r.align == 0) {
                r.sign = ROMP_SIGN_ERROR;
                mun_error(romp_sign_syntax, "`%c' is not a valid int size", s[-1]);
            }
            break;
        case ROMP_SIGN_STRUCT: {
            const char *s2 = s;
            for (struct romp_sign q; (q = romp_sign(&s2, ROMP_ACCEPT_END)).sign != ROMP_SIGN_END;) {
                if (q.sign == ROMP_SIGN_ERROR) {
                    r.sign = ROMP_SIGN_ERROR;
                    break;
                }
                r.size += q.size;
                r.align = r.align > q.align ? r.align : q.align;
            }
            break;
        }
        default:
            r.sign = ROMP_SIGN_ERROR;
            mun_error(romp_sign_syntax, "`%c' is not a known type", s[-1]);
            break;
    }
    if (r.sign != ROMP_SIGN_ERROR)
        *sign = s;
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

static int romp_encode_int(struct romp *out, int64_t in, int width) {
    return romp_encode_uint(out, (uint64_t)in, width);
}

static int romp_decode_int(struct romp *in, int64_t *out, int width) {
    uint64_t u = 0;
    if (romp_decode_uint(in, &u, width) MUN_RETHROW)
        return -1;
    *out = u >> 63 ? -(int64_t)~u - 1 : (int64_t)u;
    return 0;
}

static int romp_encode_double(struct romp *out, double in) {
    union { double f; uint64_t d; } u = { .f = in };
    return romp_encode_uint(out, u.d, 8);
}

static int romp_decode_double(struct romp *in, double *d) {
    union { double f; uint64_t d; } u = {.d = 0};
    if (romp_decode_uint(in, &u.d, 8) MUN_RETHROW)
        return -1;
    *d = u.f;
    return 0;
}

static int romp_encode_vec(struct romp *out, const char **sign, const struct mun_vec *in) {
    struct romp_sign s = romp_sign(sign, 0);
    if (s.sign == ROMP_SIGN_STRUCT)
        return mun_error(not_implemented, "romp: vectors of structs");
    if (s.sign == ROMP_SIGN_ERROR || romp_encode_uint(out, in->size, 4) MUN_RETHROW)
        return -1;
    if (s.sign == ROMP_SIGN_VEC) {
        const char *signreset = *sign;
        const struct romp_nested_vec *v = (const struct romp_nested_vec *) in;
        for mun_vec_iter(v, nv)
            if (*sign = signreset, romp_encode_vec(out, sign, nv) MUN_RETHROW)
                return -1;
        return 0;
    }
    return mun_vec_extend(out, in->data, in->size * s.size) MUN_RETHROW;
}

static int romp_decode_vec(struct romp *in, const char **sign, struct mun_vec *out) {
    uint64_t size = 0;
    struct romp_sign s = romp_sign(sign, 0);
    if (s.sign == ROMP_SIGN_STRUCT)
        return mun_error(not_implemented, "romp: vectors of structs");
    if (s.sign == ROMP_SIGN_ERROR || romp_decode_uint(in, &size, 4) MUN_RETHROW)
        return -1;
    if (mun_vec_reserve_s(s.size, out, size) MUN_RETHROW)
        return -1;
    if (s.sign == ROMP_SIGN_VEC) {
        struct romp_nested_vec *v = (struct romp_nested_vec *) out;
        for (const char *signreset = *sign; size--; ) {
            mun_vec_append(v, &(struct mun_vec){});
            if (*sign = signreset, romp_decode_vec(in, sign, &v->data[v->size - 1]) MUN_RETHROW)
                return mun_vec_fini(v), -1;
        }
    } else
        mun_vec_extend_s(s.size, out, in->data, size);
    mun_vec_erase(in, 0, s.size * size);
    return 0;
}

#define REALIGNED_TO(ptr, i) \
    (ptr = ((uintptr_t)ptr & (i - 1)) ? (__typeof__(ptr))(((uintptr_t)ptr + i) & ~(i - 1)) : ptr)

#define REALIGNED_AS(ptr, T) ((T*)REALIGNED_TO(ptr, _Alignof(T)))

static int romp_encode_struct(struct romp *out, const char **sign, const char *in, enum romp_parse_flags pf) {
    while (1) {
        struct romp_sign s = romp_sign(sign, pf);
        if (s.sign == ROMP_SIGN_NONE || s.sign == ROMP_SIGN_END)
            return 0;
        else if (s.sign == ROMP_SIGN_UINT) {
            uint64_t ur = s.size == 1 ? *REALIGNED_AS(in, const uint8_t)
                        : s.size == 2 ? *REALIGNED_AS(in, const uint16_t)
                        : s.size == 4 ? *REALIGNED_AS(in, const uint32_t)
                        : s.size == 8 ? *REALIGNED_AS(in, const uint64_t) : 0;
            if (romp_encode_uint(out, ur, s.size) MUN_RETHROW)
                return -1;
        } else if (s.sign == ROMP_SIGN_INT) {
            int64_t ir = s.size == 1 ? *REALIGNED_AS(in, const int8_t)
                       : s.size == 2 ? *REALIGNED_AS(in, const int16_t)
                       : s.size == 4 ? *REALIGNED_AS(in, const int32_t)
                       : s.size == 8 ? *REALIGNED_AS(in, const int64_t) : 0;
            if (romp_encode_int(out, ir, s.size) MUN_RETHROW)
                return -1;
        } else if (s.sign == ROMP_SIGN_DOUBLE) {
            if (romp_encode_double(out, *REALIGNED_AS(in, const double)) MUN_RETHROW)
                return -1;
        } else if (s.sign == ROMP_SIGN_VEC) {
            if (romp_encode_vec(out, sign, REALIGNED_AS(in, const struct mun_vec)) MUN_RETHROW)
                return -1;
        } else if (s.sign == ROMP_SIGN_STRUCT) {
            if (romp_encode_struct(out, sign, REALIGNED_TO(in, s.align), ROMP_ACCEPT_END) MUN_RETHROW)
                return -1;
        } else
            return -1;
        in += s.size;
    }
}

static int romp_decode_struct(struct romp *in, const char **sign, char *out, enum romp_parse_flags pf) {
    while (1) {
        struct romp_sign s = romp_sign(sign, pf);
        if (s.sign == ROMP_SIGN_NONE || s.sign == ROMP_SIGN_END)
            return 0;
        else if (s.sign == ROMP_SIGN_UINT) {
            uint64_t r = 0;
            if (romp_decode_uint(in, &r, s.size) MUN_RETHROW)
                return -1;
            if (s.size == 1) *REALIGNED_AS(out, uint8_t ) = r; else
            if (s.size == 2) *REALIGNED_AS(out, uint16_t) = r; else
            if (s.size == 4) *REALIGNED_AS(out, uint32_t) = r; else
            if (s.size == 8) *REALIGNED_AS(out, uint64_t) = r;
        } else if (s.sign == ROMP_SIGN_INT) {
            int64_t r = 0;
            if (romp_decode_int(in, &r, s.size) MUN_RETHROW)
                return -1;
            if (s.size == 1) *REALIGNED_AS(out, int8_t ) = r; else
            if (s.size == 2) *REALIGNED_AS(out, int16_t) = r; else
            if (s.size == 4) *REALIGNED_AS(out, int32_t) = r; else
            if (s.size == 8) *REALIGNED_AS(out, int64_t) = r;
        } else if (s.sign == ROMP_SIGN_DOUBLE) {
            if (romp_decode_double(in, REALIGNED_AS(out, double)) MUN_RETHROW)
                return -1;
        } else if (s.sign == ROMP_SIGN_VEC) {
            if (romp_decode_vec(in, sign, REALIGNED_AS(out, struct mun_vec)) MUN_RETHROW)
                return -1;
        } else if (s.sign == ROMP_SIGN_STRUCT) {
            if (romp_decode_struct(in, sign, REALIGNED_TO(out, s.align), ROMP_ACCEPT_END) MUN_RETHROW)
                return -1;
        } else
            return -1;
        out += s.size;
    }
}

int romp_encode(struct romp *out, const char *sign, const void *data) {
    return romp_encode_struct(out, &sign, data, ROMP_ACCEPT_NONE);
}

int romp_decode(struct romp *in, const char *sign, void *data) {
    return romp_decode_struct(in, &sign, data, ROMP_ACCEPT_NONE);
}

struct romp_signinfo romp_signinfo(const char *sign) {
    struct romp_sign s = romp_sign(&sign, ROMP_ACCEPT_NONE);
    if (s.size & (s.align - 1))
        s.size = (s.size & ~(s.align - 1)) + s.align;
    return (struct romp_signinfo){s.size, s.align};
}
