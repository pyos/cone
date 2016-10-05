#include "romp.h"

enum romp_signo
{
    ROMP_SIGN_NONE   = 0,     // signature ::= typesign* '\0'
    ROMP_SIGN_UINT   = 'u',   // typesign ::= 'u' {octets :: digit}
    ROMP_SIGN_INT    = 'i',   //            | 'd' {octets :: digit}
    ROMP_SIGN_DOUBLE = 'f',   //            | 'f'
    ROMP_SIGN_VEC    = 'v',   //            | 'v' {type :: typesign};
};

struct romp_sign
{
    enum romp_signo sign;
    unsigned stride;
};

struct romp_nested_vec veil_vec(struct veil_vec);

static struct romp_sign romp_sign(const char **sign) {
    while (1) switch ((*sign)[0]) {
        case ' ':
            (*sign)++;
            break;
        case ROMP_SIGN_NONE:
            return veil_error(romp_sign_syntax, "expected a type"), (struct romp_sign){};
        case ROMP_SIGN_DOUBLE:
            return (struct romp_sign){*(*sign)++, 8};
        case ROMP_SIGN_VEC:
            return (struct romp_sign){*(*sign)++, sizeof(struct veil_vec)};
        case ROMP_SIGN_INT:
        case ROMP_SIGN_UINT:
            if ((*sign)[1] == '1' || (*sign)[1] == '2' || (*sign)[1] == '4' || (*sign)[1] == '8')
                return (struct romp_sign){*(*sign)++, *(*sign)++ - '0'};
            return veil_error(romp_sign_syntax, "`%c' is not a valid int size", (*sign)[1]), (struct romp_sign){};
        default:
            return veil_error(romp_sign_syntax, "`%c' is not a known type", (*sign)[0]), (struct romp_sign){};
    }
}

static int romp_encode_uint(struct romp_iovec *out, uint64_t in, unsigned width) {
    uint8_t packed[] = { in >> 56, in >> 48, in >> 40, in >> 32, in >> 24, in >> 16, in >> 8, in };
    return veil_vec_extend(out, &packed[8 - width], width);
}

static int romp_decode_uint(struct romp_iovec *in, uint64_t *out, unsigned width) {
    if (in->size < width)
        return veil_error(romp_protocol, "need %u octets, found %u", width, in->size);
#define X(i) ((uint64_t)in->data[i])
    switch (width) {
        case 1: *out = X(0);  break;
        case 2: *out = X(0) << 8  | X(1); break;
        case 4: *out = X(0) << 24 | X(1) << 16 | X(2) << 8  | X(3); break;
        case 8: *out = X(0) << 56 | X(1) << 48 | X(2) << 40 | X(3) << 32 | X(4) << 24 | X(5) << 16 | X(6) << 8 | X(7); break;
//      default: __builtin_unreachable();
    }
#undef X
    veil_vec_erase(in, 0, width);
    return veil_ok;
}

static int romp_encode_int(struct romp_iovec *out, int64_t in, int width) {
    return romp_encode_uint(out, (uint64_t) in, width);
}

static int romp_decode_int(struct romp_iovec *in, int64_t *out, int width) {
    uint64_t u = 0;
    if (romp_decode_uint(in, &u, width))
        return veil_error_up();
    *out = u >= 0x80000000000000ull ? -(int64_t)~u - 1 : (int64_t)u;
    return veil_ok;
}

static int romp_encode_double(struct romp_iovec *out, double in) {
    union { double f; uint64_t d; } u = { .f = in };
    return romp_encode_uint(out, u.d, 8);
}

static int romp_decode_double(struct romp_iovec *in, double *d) {
    union { double f; uint64_t d; } u;
    if (romp_decode_uint(in, &u.d, 8))
        return veil_error_up();
    *d = u.f;
    return veil_ok;
}

static int romp_encode_vec(struct romp_iovec *out, const char **sign, const struct veil_vec *in) {
    struct romp_sign s = romp_sign(sign);
    if (!s.sign || romp_encode_uint(out, in->size, 4))
        return veil_error_up();
    if (s.sign == ROMP_SIGN_VEC) {
        const char *signreset = *sign;
        const struct romp_nested_vec *v = (const struct romp_nested_vec *) in;
        for (size_t i = 0; i < v->size; i++)
            if (*sign = signreset, romp_encode_vec(out, sign, &v->data[i]))
                return veil_error_up();
    } else if (veil_vec_extend(out, in->data, in->size * s.stride))
        return veil_error_up();
    return veil_ok;
}

static int romp_decode_vec(struct romp_iovec *in, const char **sign, struct veil_vec *out) {
    uint64_t size;
    struct romp_sign s = romp_sign(sign);
    if (!s.sign || romp_decode_uint(in, &size, 4))
        return veil_error_up();
    veil_vec_erase_s(s.stride, out, 0, out->size);
    if (veil_vec_reserve_s(s.stride, out, size))
        return veil_error_up();
    if (s.sign == ROMP_SIGN_VEC) {
        for (const char *signreset = *sign; size--; ) {
            struct veil_vec tmp = {};
            if (*sign = signreset, romp_decode_vec(in, sign, &tmp))
                return veil_error_up();
            veil_vec_splice_s(s.stride, out, out->size, &tmp, 1);
        }
    } else
        veil_vec_splice_s(s.stride, out, 0, in->data, size);
    veil_vec_erase(in, 0, s.stride * size);
    return veil_ok;
}

int romp_vencode(struct romp_iovec *out, const char *sign, va_list args) {
    int64_t ir = 0;
    uint64_t ur = 0;
    for (struct romp_sign s; *sign;) {
        switch ((s = romp_sign(&sign)).sign) {
            case ROMP_SIGN_NONE:
                return veil_error_up();
            case ROMP_SIGN_UINT:
                ur = s.stride == 4 ? va_arg(args, uint32_t)
                   : s.stride == 8 ? va_arg(args, uint64_t)
                   : va_arg(args, unsigned);
                if (romp_encode_uint(out, ur, s.stride))
                    return veil_error_up();
                break;
            case ROMP_SIGN_INT:
                ir = s.stride == 4 ? va_arg(args, int32_t)
                   : s.stride == 8 ? va_arg(args, int64_t)
                   : va_arg(args, unsigned);
                if (romp_encode_int(out, ir, s.stride))
                    return veil_error_up();
                break;
            case ROMP_SIGN_DOUBLE:
                if (romp_encode_double(out, va_arg(args, double)))
                    return veil_error_up();
                break;
            case ROMP_SIGN_VEC:
                if (romp_encode_vec(out, &sign, va_arg(args, const struct veil_vec *)))
                    return veil_error_up();
                break;
        }
    }
    return 0;
}

int romp_vdecode(struct romp_iovec *in, const char *sign, va_list args) {
    int64_t ir = 0;
    uint64_t ur = 0;
    for (struct romp_sign s; *sign;) {
        switch ((s = romp_sign(&sign)).sign) {
            case ROMP_SIGN_NONE:
                return veil_error_up();
            case ROMP_SIGN_UINT:
                if (romp_decode_uint(in, &ur, s.stride))
                    return veil_error_up();
                switch (s.stride) {
                    case 1: *va_arg(args, uint8_t  *) = ur; break;
                    case 2: *va_arg(args, uint16_t *) = ur; break;
                    case 4: *va_arg(args, uint32_t *) = ur; break;
                    case 8: *va_arg(args, uint64_t *) = ur; break;
                }
                break;
            case ROMP_SIGN_INT:
                if (romp_decode_int(in, &ir, s.stride))
                    return veil_error_up();
                switch (s.stride) {
                    case 1: *va_arg(args, int8_t  *) = ir; break;
                    case 2: *va_arg(args, int16_t *) = ir; break;
                    case 4: *va_arg(args, int32_t *) = ir; break;
                    case 8: *va_arg(args, int64_t *) = ir; break;
                }
                break;
            case ROMP_SIGN_DOUBLE:
                if (romp_decode_double(in, va_arg(args, double *)))
                    return veil_error_up();
                break;
            case ROMP_SIGN_VEC:
                if (romp_decode_vec(in, &sign, va_arg(args, struct veil_vec *)))
                    return veil_error_up();
                break;
        }
    }
    return 0;
}

int romp_encode(struct romp_iovec *out, const char *sign, ...) {
    va_list args;
    va_start(args, sign);
    int ret = romp_vencode(out, sign, args);
    va_end(args);
    return ret;
}

int romp_decode(struct romp_iovec *in, const char *sign, ...) {
    va_list args;
    va_start(args, sign);
    int ret = romp_vdecode(in, sign, args);
    va_end(args);
    return ret;
}
