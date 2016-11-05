#pragma once
#include "mun.h"

struct siy mun_vec(uint8_t);

static inline uint8_t  siy_r1(const uint8_t *p) { return p[0]; }
static inline uint16_t siy_r2(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static inline uint32_t siy_r4(const uint8_t *p) { return (uint32_t)siy_r2(p) << 16 | siy_r2(p + 2); }
static inline uint64_t siy_r8(const uint8_t *p) { return (uint64_t)siy_r4(p) << 32 | siy_r4(p + 4); }

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
//    * `vX`  - a vector of objects described by sign `X` (struct mun_vec(T));
//    * `(S)` - a naturally-aligned structure with fields described by signature S.
int siy_encode(struct siy *out, const char *sign, const void *in) mun_throws(memory, siy_sign_syntax);

// Deserialize data into a naturally-aligned structure. See `siy_encode` for a description
// of the signature. Deserialized data is erased from the input vector.
int siy_decode(struct siy *in, const char *sign, void *out) mun_throws(memory, siy_truncated, siy_sign_syntax);

// Return the size and alignment of a struct type that has a given signature.
struct siy_signinfo siy_signinfo(const char *) mun_throws(siy_sign_syntax);
