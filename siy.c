#include "siy.h"

#ifndef SIY_MAX_SIGNS
#define SIY_MAX_SIGNS 32
#endif

enum siy_signo
{
    SIY_NONE   = 0x00,  // signature ::= typesign*
    SIY_UINT   = 'u',   // typesign ::= 'u' {octets :: digit}
    SIY_INT    = 'i',   //            | 'i' {octets :: digit}
    SIY_DOUBLE = 'f',   //            | 'f'
    SIY_PTR    = '*',   //            | '*' {type :: typesign}
    SIY_VEC    = 'v',   //            | 'v' {type :: typesign}
    SIY_STRUCT = '(',   //            | '(' {contents :: signature} ')';
    SIY_END    = ')',
    SIY_ERROR  = 0xFF,
};

struct siy_sign
{
    unsigned size;
    unsigned short align;
    unsigned char sign;
    unsigned char consumes;
};

#define ALIGN(ptr, i) \
    ((ptr) = (i) ? (__typeof__(ptr))(((uintptr_t)(ptr) + ((i) - 1)) & ~(uintptr_t)((i) - 1)) : (ptr))

#define UINT_SIZE_SWITCH(s, f, default) \
    ((s) == 1 ? f(uint8_t) : (s) == 2 ? f(uint16_t) : (s) == 4 ? f(uint32_t) : (s) == 8 ? f(uint64_t) : default)

static int siy_sign(const char **in, struct siy_sign *signs, size_t size) mun_throws(siy_sign_syntax);

static int siy_sign_struct(const char **in, struct siy_sign *signs, size_t size, char end) {
    *signs = (struct siy_sign){.sign = SIY_STRUCT};
    for (struct siy_sign *next = &signs[1]; **in != end; next += next->consumes + 1) {
        if (siy_sign(in, next, size - (next - signs)))
            return -1;
        ALIGN(signs->size, next->align);
        signs->size += next->size;
        signs->align = signs->align > next->align ? signs->align : next->align;
        signs->consumes += next->consumes + 1;
    }
    ALIGN(signs->size, signs->align);
    (*in)++;
    return 0;
}

static int siy_sign_pointer(const char **in, struct siy_sign *signs, size_t size) {
    if (siy_sign(in, &signs[1], size - 1))
        return -1;
    signs[0].consumes = signs[1].consumes + 1;
    return 0;
}

static int siy_sign(const char **in, struct siy_sign *signs, size_t size) {
    while (**in && **in == ' ')
        (*in)++;
    if (size < 1)
        return mun_error(siy_sign_syntax, "cannot have more than %d signs in total", SIY_MAX_SIGNS);
    switch ((signs->sign = *(*in)++)) {
        #define SIY_SIGN_T(T) (struct siy_sign){.sign = signs->sign, .size = sizeof(T), .align = _Alignof(T)}
        case 0:
        case SIY_END:
            return mun_error(siy_sign_syntax, "mismatched parenthesis");
        case SIY_INT:
        case SIY_UINT:
            signs[0] = UINT_SIZE_SWITCH(**in - '0', SIY_SIGN_T, (struct siy_sign){});
            if (!signs[0].size)
                return mun_error(siy_sign_syntax, "invalid integer size '%c'", (*in)[-1]);
            (*in)++;
            return 0;
        case SIY_DOUBLE:
            signs[0] = SIY_SIGN_T(double);
            return 0;
        case SIY_PTR:
            signs[0] = SIY_SIGN_T(void *);
            return siy_sign_pointer(in, signs, size);
        case SIY_VEC:
            signs[0] = SIY_SIGN_T(struct mun_vec);
            return siy_sign_pointer(in, signs, size);
        case SIY_STRUCT:
            return siy_sign_struct(in, signs, size, SIY_END);
        default:
            return mun_error(siy_sign_syntax, "invalid sign '%c'", (*in)[-1]);
        #undef SIY_SIGN_T
    }
}

static int siy_signature(const char *in, struct siy_sign *signs, size_t size) {
    return siy_sign_struct(&in, signs, size, SIY_NONE);
}

static int siy_encode_uint(struct siy *out, uint64_t in, unsigned width) {
    width -= (width == 1);  // always encode 1-byte values as 1 byte
    uint8_t s = 0, r[9];
    // MSB is set if there is a next byte, except when the integer is 64-bit and there
    // are 9 bytes in the encoding; then the MSB of the last one is the MSB of
    // the original number, and there is never a 10th byte.
    do r[s++] = (in & 127) | (in > 127) << 7; while ((in >>= 7) && s <= width);
    return mun_vec_extend(out, r, s) MUN_RETHROW;
}

static int siy_decode_uint(struct siy *in, uint64_t *out, unsigned width) mun_throws(siy_truncated) {
    width -= (width == 1);
    uint8_t size = *out = 0;
    const uint8_t *i = in->data;
    do {
        if (size == in->size)
            return mun_error(siy_truncated, "could not decode an integer");
        *out |= (uint64_t)(*i & (127 | (size == width) << 7)) << (7 * size);
    } while (++size <= width && *i++ & 128);
    return mun_vec_erase(in, 0, size), 0;
}

static int siy_encode_one(struct siy *out, struct siy_sign *s, const void *in) {
    switch (s->sign) {
        case SIY_PTR:
            return siy_encode_one(out, &s[1], *(const void **)in);
        case SIY_VEC: {
            const struct mun_vec *v = in;
            if (siy_encode_uint(out, v->size, 4) MUN_RETHROW)
                return -1;
            for (unsigned i = 0; i < v->size; i++)
                if (siy_encode_one(out, &s[1], &mun_vec_data_s(s[1].size, v)[i]))
                    return -1;
            return 0;
        }
        case SIY_STRUCT:
            for (size_t i = 0; i < s->consumes; in = (char*)in + s[i + 1].size, i += s[i + 1].consumes + 1)
                if (siy_encode_one(out, &s[i + 1], ALIGN(in, s[i + 1].align)))
                    return -1;
            return 0;
        default:
        #define X(T) *(T*)in
            return siy_encode_uint(out, UINT_SIZE_SWITCH(s->size, X, 1), s->size);
        #undef X
    }
}

static int siy_decode_one(struct siy *in, struct siy_sign *s, void *out) mun_throws(siy_truncated) {
    uint64_t u = 0;
    switch (s->sign) {
        case SIY_PTR:
            return siy_decode_one(in, &s[1], *(void **)out);
        case SIY_VEC: {
            struct mun_vec *v = out;
            if (siy_decode_uint(in, &u, 4) || mun_vec_reserve_s(s[1].size, v, u) MUN_RETHROW)
                return -1;
            while (u--)
                if (siy_decode_one(in, &s[1], &mun_vec_data_s(s[1].size, v)[v->size++]))
                    return mun_vec_fini_s(s[1].size, v), -1;
            return 0;
        }
        case SIY_STRUCT:
            for (size_t i = 0; i < s->consumes; out = (char*)out + s[i + 1].size, i += s[i + 1].consumes + 1)
                if (siy_decode_one(in, &s[i + 1], ALIGN(out, s[i + 1].align)))
                    return -1;
            return 0;
        default:
            if (siy_decode_uint(in, &u, s->size))
                return -1;
        #define X(T) (*(T*)out = u)
            UINT_SIZE_SWITCH(s->size, X, 1);
        #undef X
            return 0;
    }
}

int siy_encode(struct siy *out, const char *sign, const void *in) {
    struct siy_sign signs[SIY_MAX_SIGNS];
    return siy_signature(sign, signs, SIY_MAX_SIGNS) || siy_encode_one(out, signs, in) MUN_RETHROW;
}

int siy_decode(struct siy *in, const char *sign, void *out) {
    struct siy_sign signs[SIY_MAX_SIGNS];
    return siy_signature(sign, signs, SIY_MAX_SIGNS) || siy_decode_one(in, signs, out) MUN_RETHROW;
}

struct siy_signinfo siy_signinfo(const char *sign) {
    struct siy_sign signs[SIY_MAX_SIGNS];
    if (siy_signature(sign, signs, SIY_MAX_SIGNS) MUN_RETHROW)
        return (struct siy_signinfo){};
    return (struct siy_signinfo){signs[0].size, signs[0].align};
}
