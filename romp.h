#pragma once
#include "mun.h"
#include <stdarg.h>

struct romp mun_vec(uint8_t);

enum
{
    mun_errno_romp_protocol = mun_errno_custom + 7000,
    mun_errno_romp_sign_syntax,
};

int romp_encode_var(struct romp *out, const char *sign, va_list args);
int romp_decode_var(struct romp in, const char *sign, va_list args);
int romp_encode(struct romp *out, const char *sign, ...);
int romp_decode(struct romp in, const char *sign, ...);
