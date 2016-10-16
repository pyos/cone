#pragma once
#include "mun.h"

struct romp mun_vec(uint8_t);

static inline uint8_t  romp_r1(const uint8_t *p) { return p[0]; }
static inline uint16_t romp_r2(const uint8_t *p) { return p[0] << 8 | p[1]; }
static inline uint32_t romp_r4(const uint8_t *p) { return p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline uint64_t romp_r8(const uint8_t *p) { return (uint64_t)romp_r4(p) << 32 | romp_r4(p + 4); }

enum
{
    mun_errno_romp = mun_errno_custom + 7000,
    mun_errno_romp_sign_syntax,
};

struct romp_signinfo
{
    unsigned size;
    unsigned align;
};

// Serialize a naturally-aligned structure. The signature is a sequence of optionally
// space-separated signs that describe its fields:
//    * `uN`  - an N-byte unsigned integer (uintB_t, where B = 2**N);
//    * `iN`  - an N-byte signed integer (intB_t);
//    * `f`   - a double-precision floating point number (double);
//    * `vX`  - a vector of objects described by sign `X` (struct mun_vec(T));
//    * `(S)` - a naturally-aligned structure with fields described by signature S.
//
// Errors:
//   * `romp_sign_syntax`: the signature is invalid;
//   * `memory`.
//
int romp_encode(struct romp *out, const char *sign, const void *in);

// Deserialize data into a naturally-aligned structure. See `romp_encode` for a description
// of the signature. Deserialized data is erased from the input vector.
//
// Errors:
//   * `romp`: ran out of data before decoding everything;
//   * `romp_sign_syntax`: the signature is invalid;
//   * `memory`, but only if signature contains `vX`.
//
int romp_decode(struct romp *in, const char *sign, void *out);

// Return the size and alignment required for a structure that has a given signature.
struct romp_signinfo romp_signinfo(const char *);
