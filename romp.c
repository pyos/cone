#include "romp.h"

enum romp_signo
{
    ROMP_SIGN_NONE   = 0,     // signature ::= typesign*
    ROMP_SIGN_UINT   = 'u',   // typesign ::= 'u' {octets :: digit}
    ROMP_SIGN_INT    = 'i',   //            | 'd' {octets :: digit}
    ROMP_SIGN_DOUBLE = 'f',   //            | 'f'
    ROMP_SIGN_VEC    = 'v',   //            | 'v' {type :: typesign}
    ROMP_SIGN_STRUCT = '(',   //            | '(' {contents :: signature} ')';
};

struct romp_sign
{
    enum romp_signo sign;
    unsigned stride;
};

struct romp_nested_vec mun_vec(struct mun_vec);

static struct romp_sign romp_sign(const char **sign) {
    while (1) switch ((*sign)[0]) {
        case ' ':
            (*sign)++;
            break;
        case 0:
        case ')':
            return mun_error(romp_sign_syntax, "expected a type"), (struct romp_sign){};
        case ROMP_SIGN_DOUBLE:
            return (struct romp_sign){*(*sign)++, 8};
        case ROMP_SIGN_VEC:
            return (struct romp_sign){*(*sign)++, sizeof(struct mun_vec)};
        case ROMP_SIGN_INT:
        case ROMP_SIGN_UINT:
            if ((*sign)[1] == '1' || (*sign)[1] == '2' || (*sign)[1] == '4' || (*sign)[1] == '8')
                return (struct romp_sign){*(*sign)++, *(*sign)++ - '0'};
            return mun_error(romp_sign_syntax, "`%c' is not a valid int size", (*sign)[1]), (struct romp_sign){};
        case ROMP_SIGN_STRUCT:
            return mun_error(not_implemented, "romp: struct sign"), (struct romp_sign){};
        default:
            return mun_error(romp_sign_syntax, "`%c' is not a known type", (*sign)[0]), (struct romp_sign){};
    }
}

static int romp_encode_uint(struct romp *out, uint64_t in, unsigned width) {
    uint8_t packed[] = { in >> 56, in >> 48, in >> 40, in >> 32, in >> 24, in >> 16, in >> 8, in };
    return mun_vec_extend(out, &packed[8 - width], width) MUN_RETHROW;
}

static int romp_decode_uint(struct romp *in, uint64_t *out, unsigned width) {
    if (in->size < width)
        return mun_error(romp_protocol, "need %u octets, found %u", width, in->size);
#define X(i) ((uint64_t)in->data[i])
    switch (width) {
        case 1: *out = X(0);  break;
        case 2: *out = X(0) << 8  | X(1); break;
        case 4: *out = X(0) << 24 | X(1) << 16 | X(2) << 8  | X(3); break;
        case 8: *out = X(0) << 56 | X(1) << 48 | X(2) << 40 | X(3) << 32 | X(4) << 24 | X(5) << 16 | X(6) << 8 | X(7); break;
//      default: __builtin_unreachable();
    }
#undef X
    mun_vec_erase(in, 0, width);
    return 0;
}

static int romp_encode_int(struct romp *out, int64_t in, int width) {
    return romp_encode_uint(out, (uint64_t) in, width);
}

static int romp_decode_int(struct romp *in, int64_t *out, int width) {
    uint64_t u = 0;
    if (romp_decode_uint(in, &u, width) MUN_RETHROW)
        return -1;
    *out = u >= 0x80000000000000ull ? -(int64_t)~u - 1 : (int64_t)u;
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
    struct romp_sign s = romp_sign(sign);
    if (!s.sign || romp_encode_uint(out, in->size, 4) MUN_RETHROW)
        return -1;
    if (s.sign == ROMP_SIGN_VEC) {
        const char *signreset = *sign;
        const struct romp_nested_vec *v = (const struct romp_nested_vec *) in;
        for mun_vec_iter(v, nv)
            if (*sign = signreset, romp_encode_vec(out, sign, nv) MUN_RETHROW)
                return -1;
        return 0;
    }
    return mun_vec_extend(out, in->data, in->size * s.stride) MUN_RETHROW;
}

static int romp_decode_vec(struct romp *in, const char **sign, struct mun_vec *out) {
    uint64_t size = 0;
    struct romp_sign s = romp_sign(sign);
    if (!s.sign || romp_decode_uint(in, &size, 4) MUN_RETHROW)
        return -1;
    if (out->size)
        return mun_error(assert, "non-empty vector passed to romp_decode");
    if (mun_vec_reserve_s(s.stride, out, size) MUN_RETHROW)
        return -1;
    if (s.sign == ROMP_SIGN_VEC) {
        struct romp_nested_vec *v = (struct romp_nested_vec *) out;
        for (const char *signreset = *sign; size--; ) {
            mun_vec_append(v, &(struct mun_vec){});
            if (*sign = signreset, romp_decode_vec(in, sign, &v->data[v->size - 1]) MUN_RETHROW)
                return mun_vec_fini(v), -1;
        }
    } else
        mun_vec_splice_s(s.stride, out, 0, in->data, size);
    mun_vec_erase(in, 0, s.stride * size);
    return 0;
}

int romp_encode_var(struct romp *out, const char *sign, va_list args) {
    int64_t ir = 0;
    uint64_t ur = 0;
    for (struct romp_sign s; *sign;) {
        switch ((s = romp_sign(&sign)).sign) {
            case ROMP_SIGN_NONE:
                return -1;
            case ROMP_SIGN_UINT:
                ur = s.stride == 4 ? va_arg(args, uint32_t)
                   : s.stride == 8 ? va_arg(args, uint64_t)
                   : va_arg(args, unsigned);
                if (romp_encode_uint(out, ur, s.stride) MUN_RETHROW)
                    return -1;
                break;
            case ROMP_SIGN_INT:
                ir = s.stride == 4 ? va_arg(args, int32_t)
                   : s.stride == 8 ? va_arg(args, int64_t)
                   : va_arg(args, unsigned);
                if (romp_encode_int(out, ir, s.stride) MUN_RETHROW)
                    return -1;
                break;
            case ROMP_SIGN_DOUBLE:
                if (romp_encode_double(out, va_arg(args, double)) MUN_RETHROW)
                    return -1;
                break;
            case ROMP_SIGN_VEC:
                if (romp_encode_vec(out, &sign, va_arg(args, const struct mun_vec *)) MUN_RETHROW)
                    return -1;
                break;
            case ROMP_SIGN_STRUCT:
                return mun_error(not_implemented, "romp_encode_struct");
        }
    }
    return 0;
}

int romp_decode_var(struct romp *in, const char *sign, va_list args) {
    int64_t ir = 0;
    uint64_t ur = 0;
    for (struct romp_sign s; *sign;) {
        switch ((s = romp_sign(&sign)).sign) {
            case ROMP_SIGN_NONE:
                return -1;
            case ROMP_SIGN_UINT:
                if (romp_decode_uint(in, &ur, s.stride) MUN_RETHROW)
                    return -1;
                switch (s.stride) {
                    case 1: *va_arg(args, uint8_t  *) = ur; break;
                    case 2: *va_arg(args, uint16_t *) = ur; break;
                    case 4: *va_arg(args, uint32_t *) = ur; break;
                    case 8: *va_arg(args, uint64_t *) = ur; break;
                }
                break;
            case ROMP_SIGN_INT:
                if (romp_decode_int(in, &ir, s.stride) MUN_RETHROW)
                    return -1;
                switch (s.stride) {
                    case 1: *va_arg(args, int8_t  *) = ir; break;
                    case 2: *va_arg(args, int16_t *) = ir; break;
                    case 4: *va_arg(args, int32_t *) = ir; break;
                    case 8: *va_arg(args, int64_t *) = ir; break;
                }
                break;
            case ROMP_SIGN_DOUBLE:
                if (romp_decode_double(in, va_arg(args, double *)) MUN_RETHROW)
                    return -1;
                break;
            case ROMP_SIGN_VEC:
                if (romp_decode_vec(in, &sign, va_arg(args, struct mun_vec *)) MUN_RETHROW)
                    return -1;
                break;
            case ROMP_SIGN_STRUCT:
                return mun_error(not_implemented, "romp_decode_struct");
        }
    }
    return 0;
}

int romp_encode(struct romp *out, const char *sign, ...) {
    va_list args;
    va_start(args, sign);
    int ret = romp_encode_var(out, sign, args);
    va_end(args);
    return ret;
}

int romp_decode(struct romp *in, const char *sign, ...) {
    va_list args;
    va_start(args, sign);
    int ret = romp_decode_var(in, sign, args);
    va_end(args);
    return ret;
}
