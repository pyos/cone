#pragma once
/*
 * romp / remote object messaging protocol
 *        -      -      -         -
 */
#include "cot.h"

#include <stdarg.h>

struct romp_iovec cot_vec(uint8_t);

enum romp_errno {
    cot_errno_romp_protocol = cot_errno_custom,
    cot_errno_romp_sign_syntax,
};

enum romp_signo {
    romp_sign_none   = 0,     // signature ::= typesign* '\0'
    romp_sign_uint   = 'u',   // typesign ::= 'u' {octets :: digit}
    romp_sign_int    = 'i',   //            | 'd' {octets :: digit}
    romp_sign_double = 'f',   //            | 'f'
    romp_sign_vec    = 'v',   //            | 'v' {type :: typesign};
};

struct romp_sign {
    enum romp_signo sign;
    unsigned stride;
};

static inline int romp_encode_uint(struct romp_iovec *out, uint64_t in, unsigned width) {
    uint8_t packed[] = { in >> 56, in >> 48, in >> 40, in >> 32, in >> 24, in >> 16, in >> 8, in };
    if (width != 1 && width != 2 && width != 4 && width != 8)
        return cot_error(assert, "invalid integer width");
    return cot_vec_extend(out, &packed[8 - width], width);
}

static inline int romp_decode_uint(struct romp_iovec *in, uint64_t *out, unsigned width) {
    if (in->size < width)
        return cot_error(romp_protocol, "need %u octets, found %u", width, in->size);
#define X(i) ((uint64_t)in->data[i])
    switch (width) {
        case 1: *out = X(0);  break;
        case 2: *out = X(0) << 8  | X(1); break;
        case 4: *out = X(0) << 24 | X(1) << 16 | X(2) << 8  | X(3); break;
        case 8: *out = X(0) << 56 | X(1) << 48 | X(2) << 40 | X(3) << 32 | X(4) << 24 | X(5) << 16 | X(6) << 8 | X(7); break;
        default: return cot_error(assert, "invalid uint size %u", width);
    }
#undef X
    cot_vec_erase(in, 0, width);
    return cot_ok;
}

static inline int romp_encode_int(struct romp_iovec *out, int64_t in, int width) {
    return romp_encode_uint(out, (uint64_t) in, width);
}

static inline int romp_decode_int(struct romp_iovec *in, int64_t *out, int width) {
    uint64_t u = 0;
    if (romp_decode_uint(in, &u, width))
        return cot_error_up();
    *out = u >= 0x80000000000000ull ? -(int64_t)~u - 1 : (int64_t)u;
    return cot_ok;
}

static inline int romp_encode_double(struct romp_iovec *out, double in) {
    union { double f; uint64_t d; } u = { .f = in };
    return romp_encode_uint(out, u.d, 8);
}

static inline int romp_decode_double(struct romp_iovec *in, double *d) {
    union { double f; uint64_t d; } u;
    if (romp_decode_uint(in, &u.d, 8))
        return cot_error_up();
    *d = u.f;
    return cot_ok;
}

static inline struct romp_sign romp_sign(const char **sign) {
    while (1) switch ((*sign)[0]) {
        case ' ':
            (*sign)++;
            break;
        case romp_sign_none:
            return cot_error(romp_sign_syntax, "expected a type"), (struct romp_sign){};
        case romp_sign_double:
            return (struct romp_sign){*(*sign)++, 8};
        case romp_sign_vec:
            return (struct romp_sign){*(*sign)++, 0};
        case romp_sign_int:
        case romp_sign_uint:
            if ((*sign)[1] == '1' || (*sign)[1] == '2' || (*sign)[1] == '4' || (*sign)[1] == '8')
                return (struct romp_sign){*(*sign)++, *(*sign)++ - '0'};
            return cot_error(romp_sign_syntax, "`%c' is not a valid int size", (*sign)[1]), (struct romp_sign){};
        default:
            return cot_error(romp_sign_syntax, "`%c' is not a known type", (*sign)[0]), (struct romp_sign){};
    }
}

static inline int romp_encode_vec(struct romp_iovec *out, const char **sign, const struct cot_vec *in) {
    struct romp_sign s = romp_sign(sign);
    if (!s.sign)
        return cot_error_up();
    if (s.sign == romp_sign_vec)
        return cot_error(assert, "vectors of vectors not implemented");  // TODO serialize vectors of vectors
    if (romp_encode_uint(out, in->size, 4) || cot_vec_extend(out, in->data, in->size * s.stride))
        return cot_error_up();
    return cot_ok;
}

static inline int romp_decode_vec(struct romp_iovec *in, const char **sign, struct cot_vec *out) {
    uint64_t size;
    struct romp_sign s = romp_sign(sign);
    if (!s.sign)
        return cot_error_up();
    if (s.sign == romp_sign_vec)
        return cot_error(assert, "vectors of vectors not implemented");  // TODO deserialize vectors of vectors
    cot_vec_fini_s(s.stride, out);
    if (romp_decode_uint(in, &size, 4) || cot_vec_splice_s(s.stride, out, 0, in->data, size))
        return cot_error_up();
    cot_vec_erase(in, 0, s.stride * size);
    return cot_ok;
}

static int romp_vencode(struct romp_iovec *out, const char *sign, va_list args) {
    int64_t ir = 0;
    uint64_t ur = 0;
    for (struct romp_sign s; *sign;) {
        switch ((s = romp_sign(&sign)).sign) {
            case romp_sign_none:
                return cot_error_up();
            case romp_sign_uint:
                ur = s.stride == 4 ? va_arg(args, uint32_t)
                   : s.stride == 8 ? va_arg(args, uint64_t)
                   : va_arg(args, unsigned);
                if (romp_encode_uint(out, ur, s.stride))
                    return cot_error_up();
                break;
            case romp_sign_int:
                ir = s.stride == 4 ? va_arg(args, int32_t)
                   : s.stride == 8 ? va_arg(args, int64_t)
                   : va_arg(args, unsigned);
                if (romp_encode_int(out, ir, s.stride))
                    return cot_error_up();
                break;
            case romp_sign_double:
                if (romp_encode_double(out, va_arg(args, double)))
                    return cot_error_up();
                break;
            case romp_sign_vec:
                if (romp_encode_vec(out, &sign, va_arg(args, const struct cot_vec *)))
                    return cot_error_up();
                break;
        }
    }
    return 0;
}

static int romp_vdecode(struct romp_iovec *in, const char *sign, va_list args) {
    int64_t ir = 0;
    uint64_t ur = 0;
    for (struct romp_sign s; *sign;) {
        switch ((s = romp_sign(&sign)).sign) {
            case romp_sign_none:
                return cot_error_up();
            case romp_sign_uint:
                if (romp_decode_uint(in, &ur, s.stride))
                    return cot_error_up();
                switch (s.stride) {
                    case 1: *va_arg(args, uint8_t  *) = ur; break;
                    case 2: *va_arg(args, uint16_t *) = ur; break;
                    case 4: *va_arg(args, uint32_t *) = ur; break;
                    case 8: *va_arg(args, uint64_t *) = ur; break;
                }
                break;
            case romp_sign_int:
                if (romp_decode_int(in, &ir, s.stride))
                    return cot_error_up();
                switch (s.stride) {
                    case 1: *va_arg(args, int8_t  *) = ir; break;
                    case 2: *va_arg(args, int16_t *) = ir; break;
                    case 4: *va_arg(args, int32_t *) = ir; break;
                    case 8: *va_arg(args, int64_t *) = ir; break;
                }
                break;
            case romp_sign_double:
                if (romp_decode_double(in, va_arg(args, double *)))
                    return cot_error_up();
                break;
            case romp_sign_vec:
                if (romp_decode_vec(in, &sign, va_arg(args, struct cot_vec *)))
                    return cot_error_up();
                break;
        }
    }
    return 0;
}

static int romp_encode(struct romp_iovec *out, const char *sign, ...) {
    va_list args;
    va_start(args, sign);
    int ret = romp_vencode(out, sign, args);
    va_end(args);
    return ret;
}

static int romp_decode(struct romp_iovec *in, const char *sign, ...) {
    va_list args;
    va_start(args, sign);
    int ret = romp_vdecode(in, sign, args);
    va_end(args);
    return ret;
}
