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

// Write a serialized version of arguments into a vector. The signature is a string
// with (optionally space-separated) type signs: `uN` for an N-byte unsigned integer,
// where N is 1, 2, 4, or 8 (uintN_t); `iN` for an N-byte signed integer (intN_t);
// `f` for a double-precision floating point number (double); `vX` for a vector of
// elements described by sign `X` (struct mun_vec(T)); `(X)` for a structure that
// contains naturally-aligned fields of types desribed by signature `X`.
// Nested vectors are allowed.
//
// Errors:
//   * `romp_sign_syntax`: the signature is invalid;
//   * `memory`.
//
int romp_encode(struct romp *out, const char *sign, const void *);

// Decode serialized values into storage pointed to by arguments. The signature is the same
// as for `romp_encode`. Data from the input vector is erased as arguments are decoded.
//
// Errors:
//   * `romp`: ran out of data before decoding everything;
//   * `romp_sign_syntax`: the signature is invalid;
//   * `memory`, but only if signature contains `vX`.
//
int romp_decode(struct romp *in, const char *sign, void *);

struct romp_signinfo
{
    unsigned size;
    unsigned align;
};

// Return the size and alignment required for a structure that has a given signature.
struct romp_signinfo romp_signinfo(const char *);
