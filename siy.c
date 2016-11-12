#include "siy.h"

enum siy_signo          // signature ::= (' '* typesign)*
{
    SIY_UINT   = 'u',   // typesign ::= 'u' [1248]
    SIY_INT    = 'i',   //            | 'i' [1248]
    SIY_DOUBLE = 'f',   //            | 'f'
    SIY_PTR    = '*',   //            | '*' ' '* typesign
    SIY_VEC    = 'v',   //            | 'v' ' '* typesign
    SIY_STRUCT = '(',   //            | '(' signature ')';
};

#define ALIGN(ptr, i) \
    ((ptr) = (__typeof__(ptr))(((uintptr_t)(ptr) + ((i) - 1)) & ~(uintptr_t)((i) - 1)))

#define UINT_SIZE_SWITCH(s, f, default) \
    ((s) == 1 ? f(uint8_t) : (s) == 2 ? f(uint16_t) : (s) == 4 ? f(uint32_t) : (s) == 8 ? f(uint64_t) : default)

static int siy_sign(const char **in, struct siy_sign *sgn, size_t n) mun_throws(siy_sign_syntax);

static int siy_sign_struct(const char **in, struct siy_sign *sgn, size_t n, char end) {
    *sgn = (struct siy_sign){.sign = SIY_STRUCT, .align = 1};
    while (**in != end) {
        struct siy_sign *next = &sgn[sgn->consumes + 1];
        if (siy_sign(in, next, n - sgn->consumes - 1))
            return -1;
        ALIGN(sgn->size, next->align);
        if (sgn->align < next->align)
            sgn->align = next->align;
        sgn->size += next->size;
        sgn->consumes += next->consumes + 1;
    }
    (*in)++;
    ALIGN(sgn->size, sgn->align);
    return 0;
}

static int siy_sign(const char **in, struct siy_sign *sgn, size_t n) {
    if (n < 1)
        return mun_error(siy_sign_syntax, "signature too big");
    while (**in && **in == ' ')
        (*in)++;
    switch ((sgn->sign = *(*in)++)) {
        case '\0':
            return mun_error(siy_sign_syntax, "unexpected end of signature");
        case ')':
            return mun_error(siy_sign_syntax, "mismatched parenthesis");
        #define SIY_SIGN_T(T) (sgn->consumes = 0, sgn->align = _Alignof(T), sgn->size = sizeof(T))
        case SIY_INT:
        case SIY_UINT:
            if (!UINT_SIZE_SWITCH(**in - '0', SIY_SIGN_T, 0))
                return mun_error(siy_sign_syntax, "invalid integer size '%c'", **in);
            (*in)++;
            return 0;
        case SIY_DOUBLE:
            SIY_SIGN_T(double);
            return 0;
        case SIY_PTR:
            SIY_SIGN_T(void *);
            if (0)
        case SIY_VEC:
                SIY_SIGN_T(struct mun_vec);
            if (siy_sign(in, &sgn[1], n - 1))
                return -1;
            sgn->consumes = sgn[1].consumes + 1;
            return 0;
        case SIY_STRUCT:
            return siy_sign_struct(in, sgn, n, ')');
        #undef SIY_SIGN_T
    }
    return mun_error(siy_sign_syntax, "invalid sign '%c'", sgn->sign);
}

int siy_signature(const char *in, struct siy_sign *sgn, size_t n) {
    return siy_sign_struct(&in, sgn, n, '\0');
}

static unsigned bitcount(uint64_t i) {
    unsigned r = 0; while (i) i >>= 1, r++; return r;
}

static int siy_encode_uint(struct siy *out, uint64_t in, unsigned width) {
    if (width <= 1 || in < 0x80)
        return mun_vec_extend(out, ((uint8_t[]){in}), 1);
    uint8_t s = (bitcount(in) + 3) / 8;
    uint8_t r[9] = {0x80 | (s - 1) << 4 | (in & 15), in >> 4,  in >> 12, in >> 20, in >> 28,
                                                     in >> 36, in >> 44, in >> 52, in >> 60};
    return mun_vec_extend(out, r, s + 1) MUN_RETHROW;
}

static int siy_decode_uint(struct siy *in, uint64_t *out, unsigned width) mun_throws(siy_truncated) {
    uint8_t s = 0;
    if (in->size < 1)
        return mun_error(siy_truncated, "could not decode an integer");
    *out = in->data[0];
    if (width > 1 && *out > 0x80 && in->size < (s = (*out >> 4 & 7) + 1))
        return mun_error(siy_truncated, "could not decode an integer");
    if (s)
        *out &= 15;
    for (uint8_t i = 0; i < s; i++)
        *out |= (uint64_t)in->data[i + 1] << (8 * i + 4);
    return mun_vec_erase(in, 0, s + 1), 0;
}

int siy_encode_s(struct siy *out, const struct siy_sign *s, const void *in) {
    for (; s->sign == SIY_PTR; s++)
        in = *(const void *const *)in;
    switch (s->sign) {
        case SIY_VEC: {
            const struct mun_vec *v = in;
            if (siy_encode_uint(out, v->size, 4) MUN_RETHROW)
                return -1;
            for (unsigned i = 0; i < v->size; i++)
                if (siy_encode_s(out, &s[1], &mun_vec_data_s(s[1].size, v)[i]))
                    return -1;
            return 0;
        }
        case SIY_STRUCT:
            for (size_t i = 0; i < s->consumes; in = (const char*)in + s[i + 1].size, i += s[i + 1].consumes + 1)
                if (siy_encode_s(out, &s[i + 1], ALIGN(in, s[i + 1].align)))
                    return -1;
            return 0;
        default:
        #define X(T) *(T*)in
            return siy_encode_uint(out, UINT_SIZE_SWITCH(s->size, X, 1), s->size);
        #undef X
    }
}

int siy_decode_s(struct siy *in, const struct siy_sign *s, void *out) {
    for (; s->sign == SIY_PTR; s++)
        out = *(void **)out;
    switch (s->sign) {
            uint64_t u;
        case SIY_VEC:
            if (siy_decode_uint(in, &u, 4) || mun_vec_reserve_s(s[1].size, out, u) MUN_RETHROW)
                return -1;
            while (u--)
                if (siy_decode_s(in, &s[1], &mun_vec_data_s(s[1].size, out)[((struct mun_vec *)out)->size++]))
                    return mun_vec_fini_s(s[1].size, out), -1;
            return 0;
        case SIY_STRUCT:
            for (size_t i = 0; i < s->consumes; out = (char*)out + s[i + 1].size, i += s[i + 1].consumes + 1)
                if (siy_decode_s(in, &s[i + 1], ALIGN(out, s[i + 1].align)))
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
