#pragma once
#include "mun.h"
#include <stdarg.h>

struct romp_iovec mun_vec(uint8_t);

enum
{
    mun_errno_romp_protocol = mun_errno_custom,
    mun_errno_romp_sign_syntax,
};

int romp_vencode(struct romp_iovec *out, const char *sign, va_list args);
int romp_vdecode(struct romp_iovec *in, const char *sign, va_list args);
int romp_encode(struct romp_iovec *out, const char *sign, ...);
int romp_decode(struct romp_iovec *in, const char *sign, ...);