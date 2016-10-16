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
    ROMP_ACCEPT_ONE  = 0x4,
};

struct romp_sign
{
    enum romp_signo sign;
    unsigned size;
    unsigned align;
    const char *contents;
    const char *next;
};

struct romp_nested_vec mun_vec(struct mun_vec);

static struct romp_sign romp_sign(const char *sign, enum romp_parse_flags flags) {
    while (*sign && *sign == ' ')
        sign++;
    struct romp_sign r = {};
    switch ((r.sign = *sign++)) {
        case ROMP_SIGN_NONE:
            if (flags & ROMP_ACCEPT_NONE)
                break;
            mun_error(romp_sign_syntax, "unexpected end of signature");
            goto fail;
        case ROMP_SIGN_END:
            if (flags & ROMP_ACCEPT_END)
                break;
            mun_error(romp_sign_syntax, "unexpected end of struct");
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
        case ROMP_SIGN_INT:
        case ROMP_SIGN_UINT:
            r.size = (unsigned)*sign++ - '0';
            r.align = r.size == 1 ? _Alignof(uint8_t)
                    : r.size == 2 ? _Alignof(uint16_t)
                    : r.size == 4 ? _Alignof(uint32_t)
                    : r.size == 8 ? _Alignof(uint64_t) : 0;
            if (r.align)
                break;
            mun_error(romp_sign_syntax, "`%c' is not a valid int size", sign[-1]);
            goto fail;
        case ROMP_SIGN_STRUCT: {
            r.contents = sign;
            struct romp_sign field;
            while ((field = romp_sign(sign, ROMP_ACCEPT_END)).sign != ROMP_SIGN_END) {
                if (field.sign == ROMP_SIGN_ERROR)
                    goto fail;
                if (r.size & (field.align - 1))
                    r.size = (r.size & ~(field.align - 1)) + field.align;
                r.size += field.size;
                r.align = r.align > field.align ? r.align : field.align;
                sign = field.next;
            }
            if (r.size & (r.align - 1))
                r.size = (r.size & ~(r.align - 1)) + r.align;
            sign = field.next;
            break;
        }
        default:
            mun_error(romp_sign_syntax, "`%c' is not a known type", sign[-1]);
            goto fail;
    }
    r.next = sign;
    return r;
fail:
    r.sign = ROMP_SIGN_ERROR;
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

static int romp_encode_struct(struct romp *, const char *sign, const char *, enum romp_parse_flags);
static int romp_decode_struct(struct romp *, const char *sign, char *, enum romp_parse_flags);

static int romp_encode_vec(struct romp *out, const char *vtype, const struct mun_vec *in) {
    struct romp_sign s = romp_sign(vtype, 0);
    if (s.sign == ROMP_SIGN_ERROR || romp_encode_uint(out, in->size, 4) MUN_RETHROW)
        return -1;
    for (const char *it = in->data, *end = in->data + s.size * in->size; it != end; it += s.size)
        if (romp_encode_struct(out, vtype, it, ROMP_ACCEPT_ONE) MUN_RETHROW)
            return -1;
    return 0;
}

static int romp_decode_vec(struct romp *in, const char *vtype, struct mun_vec *out) {
    uint64_t size = 0;
    struct romp_sign s = romp_sign(vtype, 0);
    if (s.sign == ROMP_SIGN_ERROR || romp_decode_uint(in, &size, 4) MUN_RETHROW)
        return -1;
    if (mun_vec_reserve_s(s.size, out, size) MUN_RETHROW)
        return -1;
    while (size--)
        if (romp_decode_struct(in, vtype, &out->data[out->size++ * s.size], ROMP_ACCEPT_ONE) MUN_RETHROW)
            return mun_vec_fini(out), -1;
    return 0;
}

#define REALIGNED_TO(ptr, i) \
    (ptr = ((uintptr_t)ptr & (i - 1)) ? (__typeof__(ptr))(((uintptr_t)ptr + i) & ~(i - 1)) : ptr)

#define REALIGNED_AS(ptr, T) ((T*)REALIGNED_TO(ptr, _Alignof(T)))

static int romp_encode_struct(struct romp *out, const char *sign, const char *in, enum romp_parse_flags pf) {
    do {
        struct romp_sign s = romp_sign(sign, pf);
        if (s.sign == ROMP_SIGN_ERROR MUN_RETHROW)
            return -1;
        else if (s.sign == ROMP_SIGN_NONE || s.sign == ROMP_SIGN_END)
            break;
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
            if (romp_encode_vec(out, s.contents, REALIGNED_AS(in, const struct mun_vec)) MUN_RETHROW)
                return -1;
        } else if (s.sign == ROMP_SIGN_STRUCT) {
            if (romp_encode_struct(out, s.contents, REALIGNED_TO(in, s.align), ROMP_ACCEPT_END) MUN_RETHROW)
                return -1;
        }
        in += s.size;
        sign = s.next;
    } while (!(pf & ROMP_ACCEPT_ONE));
    return 0;
}

static int romp_decode_struct(struct romp *in, const char *sign, char *out, enum romp_parse_flags pf) {
    do {
        struct romp_sign s = romp_sign(sign, pf);
        if (s.sign == ROMP_SIGN_ERROR MUN_RETHROW)
            return -1;
        else if (s.sign == ROMP_SIGN_NONE || s.sign == ROMP_SIGN_END)
            break;
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
            if (romp_decode_vec(in, s.contents, REALIGNED_AS(out, struct mun_vec)) MUN_RETHROW)
                return -1;
        } else if (s.sign == ROMP_SIGN_STRUCT) {
            if (romp_decode_struct(in, s.contents, REALIGNED_TO(out, s.align), ROMP_ACCEPT_END) MUN_RETHROW)
                return -1;
        }
        out += s.size;
        sign = s.next;
    } while (!(pf & ROMP_ACCEPT_ONE));
    return 0;
}

int romp_encode(struct romp *out, const char *sign, const void *data) {
    return romp_encode_struct(out, sign, data, ROMP_ACCEPT_NONE);
}

int romp_decode(struct romp *in, const char *sign, void *data) {
    return romp_decode_struct(in, sign, data, ROMP_ACCEPT_NONE);
}

struct romp_signinfo romp_signinfo(const char *sign) {
    struct romp_sign s = romp_sign(sign, ROMP_ACCEPT_NONE);
    return (struct romp_signinfo){s.size, s.align};
}
