#pragma once
#include "cone.h"

#include <stdarg.h>

struct romp;

struct romp_closure
{
    const char *name;
    int (*code)(struct romp *, void *, ...);
    void *data;
};

struct romp
{
    unsigned time;
    struct romp_closure methods[127];
};

struct romp_iovec cone_vec(uint8_t);

enum ROMP_SIGN           // signature ::= {typesign}* '\0'
{
    ROMP_UINT   = 'u',   // typesign ::= ROMP_UINT {octets :: digit}
    ROMP_INT    = 'd',   //            | ROMP_INT  {octets :: digit}
    ROMP_DOUBLE = 'f',   //            | ROMP_DOUBLE
    ROMP_VEC    = 'v',   //            | ROMP_VEC {type :: typesign}
};

static inline int
romp_encode_uint(struct romp_iovec *out, uint64_t in, unsigned width) {
    uint8_t packed[] = { in >> 56, in >> 48, in >> 40, in >> 32, in >> 24, in >> 16, in >> 8, in };
    if (width != 1 && width != 2 && width != 4 && width != 8)
        return -1;
    return cone_vec_extend(out, packed + 8 - width, width);
}

static inline int
romp_decode_uint(struct romp_iovec *in, uint64_t *out, unsigned width) {
    if (in->size < width)
        return -1;
#define X(i) ((uint64_t)in->data[i])
    switch (width) {
        case 1: *out = X(0);  break;
        case 2: *out = X(0) << 8  | X(1); break;
        case 4: *out = X(0) << 24 | X(1) << 16 | X(2) << 8  | X(3); break;
        case 8: *out = X(0) << 56 | X(1) << 48 | X(2) << 40 | X(3) << 32 | X(4) << 24 | X(5) << 16 | X(6) << 8 | X(7); break;
        default: return -1;
    }
#undef X
    cone_vec_erase(in, 0, width);
    return 0;
}

static inline int
romp_encode_int(struct romp_iovec *out, int64_t in, int width) {
    return romp_encode_uint(out, (uint64_t) in, width);
}

static inline int
romp_decode_int(struct romp_iovec *in, int64_t *out, int width) {
    uint64_t u = 0;
    if (romp_decode_uint(in, &u, width))
        return -1;
    *out = u >= 0x80000000000000ull ? -(int64_t)(u & 0x7FFFFFFFFFFFFFFFull) : (int64_t)u;
    return 0;
}

static inline int
romp_encode_double(struct romp_iovec *out, double in) {
    union { double f; uint64_t d; } u = { .f = in };
    return romp_encode_uint(out, u.d, 8);
}

static inline int
romp_decode_double(struct romp_iovec *in, double *d) {
    union { double f; uint64_t d; } u;
    if (romp_decode_uint(in, &u.d, 8))
        return -1;
    *d = u.f;
    return 0;
}

struct romp_stride_info
{
    unsigned stride;
    unsigned sign_length;
};

static inline struct romp_stride_info
romp_stride_of(const char *sign) {
    switch (*sign++) {
        case ROMP_DOUBLE:
            return (struct romp_stride_info){8, 1};
        case ROMP_VEC:
            return (struct romp_stride_info){0, 1};
        case ROMP_INT:
        case ROMP_UINT:
            if (*sign == '1' || *sign == '2' || *sign == '4' || *sign == '8')
                return (struct romp_stride_info){*sign - '0', 2};
        default:
            return (struct romp_stride_info){0, 0};
    }
}

static inline int
romp_encode_vec(struct romp_iovec *out, const char **sign, const struct cone_vec *in) {
    struct romp_stride_info si = romp_stride_of(*sign);
    if (si.sign_length == 0)
        return -1;
    if (si.stride == 0)
        return -1;  // TODO serialize vectors of vectors
    if (romp_encode_uint(out, in->size, 4))
        return -1;
    if (cone_vec_extend(out, in->data, in->size * si.stride))
        return -1;
    *sign += si.sign_length;
    return 0;
}

static inline int
romp_decode_vec(struct romp_iovec *in, const char **sign, struct cone_vec *out) {
    uint64_t size;
    struct romp_stride_info si = romp_stride_of(*sign);
    if (si.sign_length == 0)
        return -1;
    if (si.stride == 0)
        return -1;  // TODO deserialize vectors of vectors
    if (romp_decode_uint(in, &size, 4))
        return -1;
    cone_vec_fini_s(si.stride, out);
    if (cone_vec_splice_s(si.stride, out, 0, in->data, size))
        return -1;
    *sign += si.sign_length;
    return 0;
}

static int
romp_vencode(struct romp_iovec *out, const char *sign, va_list args) {
    while (*sign) switch (*sign++) {
        case ROMP_UINT: {
            uint64_t r = 0;
            switch (*sign) {
                case '1': r = va_arg(args, unsigned);
                case '2': r = va_arg(args, unsigned);
                case '4': r = va_arg(args, unsigned long);
                case '8': r = va_arg(args, unsigned long long);
                default: return -1;
            }
            if (romp_encode_uint(out, r, *sign++ - '0'))
                return -1;
            break;
        }
        case ROMP_INT: {
            int64_t r = 0;
            switch (*sign) {
                case '1': r = va_arg(args, signed);
                case '2': r = va_arg(args, signed);
                case '4': r = va_arg(args, signed long);
                case '8': r = va_arg(args, signed long long);
                default: return -1;
            }
            if (romp_encode_int(out, r, *sign++ - '0'))
                return -1;
            break;
        }
        case ROMP_DOUBLE:
            if (romp_encode_double(out, va_arg(args, double)))
                return -1;
            break;
        case ROMP_VEC:
            if (romp_encode_vec(out, &sign, va_arg(args, const struct cone_vec *)))
                return -1;
            break;
        default: return -1;
    }
    return 0;
}

static int
romp_vdecode(struct romp_iovec *in, const char *sign, va_list args) {
    while (*sign) switch (*sign++) {
        case ROMP_UINT: {
            uint64_t r = 0;
            if (romp_decode_uint(in, &r, *sign - '0'))
                return -1;
            switch (*sign++) {
                case '1': *va_arg(args, uint8_t  *) = r;
                case '2': *va_arg(args, uint16_t *) = r;
                case '4': *va_arg(args, uint32_t *) = r;
                case '8': *va_arg(args, uint64_t *) = r;
                default: return -1;
            }
            break;
        }
        case ROMP_INT: {
            int64_t r = 0;
            if (romp_decode_int(in, &r, *sign - '0'))
                return -1;
            switch (*sign++) {
                case '1': *va_arg(args, int8_t  *) = r;
                case '2': *va_arg(args, int16_t *) = r;
                case '4': *va_arg(args, int32_t *) = r;
                case '8': *va_arg(args, int64_t *) = r;
                default: return -1;
            }
            break;
        }
        case ROMP_DOUBLE:
            if (romp_decode_double(in, va_arg(args, double *)))
                return -1;
            break;
        case ROMP_VEC:
            if (romp_decode_vec(in, &sign, va_arg(args, struct cone_vec *)))
                return -1;
            break;
        default: return -1;
    }
    return 0;
}

static int
romp_encode(struct romp_iovec *out, const char *sign, ...) {
    va_list args;
    va_start(args, sign);
    int ret = romp_vencode(out, sign, args);
    va_end(args);
    return ret;
}

static int
romp_decode(struct romp_iovec *in, const char *sign, ...) {
    va_list args;
    va_start(args, sign);
    int ret = romp_vdecode(in, sign, args);
    va_end(args);
    return ret;
}
