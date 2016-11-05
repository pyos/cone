#include "siy.h"

enum siy_signo
{
    SIY_NONE   = 0,     // signature ::= typesign*
    SIY_ERROR  = 1,
    SIY_UINT   = 'u',   // typesign ::= 'u' {octets :: digit}
    SIY_INT    = 'i',   //            | 'i' {octets :: digit}
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

#define UINT_SIZE_SWITCH(s, f) \
    (s == 1 ? f(uint8_t) : s == 2 ? f(uint16_t) : s == 4 ? f(uint32_t) : s == 8 ? f(uint64_t) : 0)

static struct siy_sign siy_sign(const char *sign, int accept_end) mun_throws(siy_sign_syntax) {
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
            r.align = UINT_SIZE_SWITCH(r.size, _Alignof);
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
        *out |= (*i & (127ull | (size == width) << 7)) << (7 * size);
    } while (++size <= width && *i++ & 128);
    return mun_vec_erase(in, 0, size), 0;
}

static int siy_encode_one(struct siy *out, struct siy_sign s, const void *in) {
    switch (s.sign) {
        case SIY_INT:
        case SIY_UINT:
        case SIY_DOUBLE:
        #define X(t) *(t*)in
            if (siy_encode_uint(out, UINT_SIZE_SWITCH(s.size, X), s.size) MUN_RETHROW)
                return -1;
        #undef X
            return 0;
        case SIY_VEC: {
            struct siy_sign q = siy_sign(s.contents, 0);
            const struct mun_vec *v = in;
            if (q.sign == SIY_ERROR || siy_encode_uint(out, v->size, 4) MUN_RETHROW)
                return -1;
            for (unsigned i = 0; i < v->size; i++)
                if (siy_encode_one(out, q, &mun_vec_data_s(q.size, v)[i]) MUN_RETHROW)
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

static int siy_decode_one(struct siy *in, struct siy_sign s, void *out) mun_throws(siy_truncated) {
    uint64_t u = 0;
    switch (s.sign) {
        case SIY_INT:
        case SIY_UINT:
        case SIY_DOUBLE:
            if (siy_decode_uint(in, &u, s.size) MUN_RETHROW)
                return -1;
        #define X(t) (*(t*)out = u)
            UINT_SIZE_SWITCH(s.size, X);
        #undef X
            return 0;
        case SIY_VEC: {
            struct siy_sign q = siy_sign(s.contents, 0);
            struct mun_vec *v = out;
            if (q.sign == SIY_ERROR || siy_decode_uint(in, &u, 4) MUN_RETHROW)
                return -1;
            if (mun_vec_reserve_s(q.size, v, u) MUN_RETHROW)
                return -1;
            while (u--)
                if (siy_decode_one(in, q, &mun_vec_data_s(q.size, v)[v->size++]) MUN_RETHROW)
                    return mun_vec_fini_s(q.size, v), -1;
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
    const char *in_ = in;
    for (struct siy_sign s; (s = siy_sign(sign, 1)).size; in_ += s.size, sign = s.next)
        if (s.sign == SIY_ERROR || siy_encode_one(out, s, ALIGN(in_, s.align)) MUN_RETHROW)
            return -1;
    return 0;
}

int siy_decode(struct siy *in, const char *sign, void *out) {
    char *out_ = out;
    for (struct siy_sign s; (s = siy_sign(sign, 1)).size; out_ += s.size, sign = s.next)
        if (s.sign == SIY_ERROR || siy_decode_one(in, s, ALIGN(out_, s.align)) MUN_RETHROW)
            return -1;
    return 0;
}

struct siy_signinfo siy_signinfo(const char *sign) {
    struct siy_signinfo si = {};
    for (struct siy_sign s; (s = siy_sign(sign, 1)).sign != SIY_NONE;) {
        if (s.sign == SIY_ERROR || s.sign == SIY_END)
            return (struct siy_signinfo){};
        ALIGN(si.size, s.align);
        si.size += s.size;
        si.align = s.align > si.align ? s.align : si.align;
        sign = s.next;
    }
    return si;
}
