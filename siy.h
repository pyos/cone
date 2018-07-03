#pragma once
#include "mun.h"

#ifndef SIY_MAX_SIGNS
#define SIY_MAX_SIGNS 32
#endif

#if __cplusplus
extern "C" {
#endif

struct siy mun_vec(uint8_t);

enum
{
    mun_errno_siy_truncated = mun_errno_custom + 7000,
    mun_errno_siy_sign_syntax,
};

struct siy_sign
{
    unsigned size;
    unsigned short align;
    unsigned char sign;
    unsigned char consumes;
};

// Parse a signature, which is a sequence of at most `n-1` (optionally space-separated)
// signs that describe a naturally-aligned structure's layout:
//    * `uN`  - an N-byte unsigned integer (uintB_t, where B = 8N);
//    * `iN`  - an N-byte signed integer (intB_t);
//    * `f`   - a double-precision floating point number (double);
//    * `*X`  - a pointer to a naturally-aligned structure described by sign `X` (T*);
//    * `vX`  - a vector of objects described by sign `X` (struct mun_vec(T));
//    * `(S)` - a naturally-aligned structure with fields described by signature S.
// The zeroth sign is reserved for the implicit structure wrapping the whole signature.
int siy_signature(const char *, struct siy_sign *, size_t n) mun_throws(siy_sign_syntax);

// Serialize a naturally-aligned structure that conforms to a signature.
int siy_encode_s(struct siy *out, const struct siy_sign *, const void *in) mun_throws(memory);

// Deserialize data into a naturally-aligned structure. Deserialied data is erased
// from the input vector. If the signature includes `*X`, the field must already point
// to valid storage for type described by `X`. The field corresponding to `vX` may be
// pre-initialized with a static vector; otherwise, a new dynamic vector will be created.
int siy_decode_s(struct siy *in, const struct siy_sign *, void *out) mun_throws(memory, siy_truncated);

// Parse a signature and serialize a structure that conforms to it.
static inline int siy_encode(struct siy *out, const char *sign, const void *in) mun_throws(memory, siy_sign_syntax) {
    struct siy_sign s[SIY_MAX_SIGNS];
    return siy_signature(sign, s, SIY_MAX_SIGNS) || siy_encode_s(out, s, in) MUN_RETHROW;
}

// Parse a signature and deserialize a structure that conforms to it.
static inline int siy_decode(struct siy *in, const char *sign, void *out) mun_throws(memory, siy_truncated, siy_sign_syntax) {
    struct siy_sign s[SIY_MAX_SIGNS];
    return siy_signature(sign, s, SIY_MAX_SIGNS) || siy_decode_s(in, s, out) MUN_RETHROW;
}

#if __cplusplus
} // extern "C"
#endif
