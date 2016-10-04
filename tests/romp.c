#include "../romp.h"

#include <stdio.h>
#include <inttypes.h>

int enctest(struct romp_iovec *out) {
    uint8_t  u8  = 8;
    uint16_t u16 = 16;
    uint32_t u32 = 32;
    uint64_t u64 = 64;
    int8_t   i8  = -8;
    int16_t  i16 = -16;
    int32_t  i32 = -32;
    int64_t  i64 = -64;
    double   dbl = 1234.5678;
    struct veil_vec(char) str = {.data = "Hello, World!\n", .size = 14};

    if (romp_encode(out, "u1 u2 u4 u8 f vi1 i1 i2 i4 i8", u8, u16, u32, u64, dbl, &str, i8, i16, i32, i64))
        return veil_error_up();
    return veil_ok;
}

int dectest(struct romp_iovec *in) {
    uint8_t  u8  = 0;
    uint16_t u16 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    int8_t   i8  = 0;
    int16_t  i16 = 0;
    int32_t  i32 = 0;
    int64_t  i64 = 0;
    double   dbl = 0;
    struct veil_vec(char) str = {};
    if (romp_decode(in, "u1 u2 u4 u8 f vi1 i1 i2 i4 i8", &u8, &u16, &u32, &u64, &dbl, &str, &i8, &i16, &i32, &i64))
        return veil_error_up();
    printf("u8  = %u\n", u8);
    printf("u16 = %u\n", u16);
    printf("u32 = %u\n", u32);
    printf("u64 = %" PRIu64 "\n", u64);
    printf("i8  = %d\n", i8);
    printf("i16 = %d\n", i16);
    printf("i32 = %d\n", i32);
    printf("i64 = %" PRIi64 "\n", i64);
    printf("dbl = %f\n", dbl);
    printf("str = %.*s\n", (int)str.size, str.data);
    return veil_ok;
}

int comain() {
    struct romp_iovec vec = {};
    if (enctest(&vec))
        return veil_error_up();
    struct romp_iovec rd = vec;
    if (dectest(&rd))
        return veil_error_up();
    return 0;
}
