#pragma once
#include "mun.h"

struct siy mun_vec(uint8_t);

enum
{
    mun_errno_siy_truncated = mun_errno_custom + 7000,
    mun_errno_siy_sign_syntax,
};

struct siy_signinfo
{
    unsigned size;
    unsigned align;
};

// Serialize a naturally-aligned structure. The signature is a sequence of optionally
// space-separated signs that describe its fields:
//    * `uN`  - an N-byte unsigned integer (uintB_t, where B = 8N);
//    * `iN`  - an N-byte signed integer (intB_t);
//    * `f`   - a double-precision floating point number (double);
//    * `*X`  - a pointer to a naturally-aligned structure described by sign `X` (T*);
//    * `vX`  - a vector of objects described by sign `X` (struct mun_vec(T));
//    * `(S)` - a naturally-aligned structure with fields described by signature S.
int siy_encode(struct siy *out, const char *sign, const void *in) mun_throws(memory, siy_sign_syntax);

// Deserialize data into a naturally-aligned structure. See `siy_encode` for a description
// of the signature. If the signature includes `*X`, the field must already point to valid
// storage for type described by `X`. The field corresponding to `vX` may be pre-initialized
// with a static vector; otherwise, a new dynamic vector will be created.
int siy_decode(struct siy *in, const char *sign, void *out) mun_throws(memory, siy_truncated, siy_sign_syntax);

// Return the size and alignment of a struct type that has a given signature.
struct siy_signinfo siy_signinfo(const char *) mun_throws(siy_sign_syntax);
