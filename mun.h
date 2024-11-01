#pragma once
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __cplusplus
extern "C" {
#endif

enum {
    // Read operation timed out. (ETIMEDOUT is specified by POSIX to be the connect() timeout.)
    ERTIMEDOUT = 20113,
    // Write operation timed out.
    EWTIMEDOUT = 20114,
    // A C++ exception has occurred.
    EEXCEPTION = 20519,
};

// Tagged pointer support: `mun_tag_ptr(mun_tag_add(p, tag)) == p` and
// `mun_tag_get(mun_tag_add(p, tag)) == tag`. Since alignment is only known for complete
// types, incomplete types should be used with `_aligned` versions of these functions.
// Using incorrect alignment or calling `mun_tag_add` with a tag larger than the pointer's
// alignment is UB.
#define mun_tag_add(p, tag) ((__typeof__(p))((uintptr_t)(p) | (tag)))
#define mun_tag_get_aligned(p, align) ((uintptr_t)(p) & ((align) - 1))
#define mun_tag_ptr_aligned(p, align) ((__typeof__(p))((uintptr_t)(p) & ~(uintptr_t)((align) - 1)))
#define mun_tag_get(p) mun_tag_get_aligned(p, _Alignof(__typeof__(*p)))
#define mun_tag_ptr(p) mun_tag_ptr_aligned(p, _Alignof(__typeof__(*p)))

// A microsecond-resolution clock. That's good enough; `epoll_wait` can't handle
// less than millisecond resolution anyway.
typedef int64_t mun_usec;

#define MUN_USEC_MAX INT64_MAX

// The wall clock time. May jump around depending on the whims of NTP servers.
mun_usec mun_usec_now(void);

// The monotonic clock. Only the differences are meaningful, but it always moves forward
// at a rate of one second per second.
mun_usec mun_usec_monotonic(void);

struct mun_stackframe {
    const char *file;
    const char *func;
    unsigned line;
};

struct mun_error {
    int code;
    unsigned stacklen;
    const char *name;
    char text[256];
    // Stack at the time of the error, starting from innermost frame. Aside from the
    // origin, only frames in which `mun_error_up` (aka `MUN_RETHROW`) was evaluated are
    // recorded. Meaning, this is not a complete stack trace, but it sure is fast.
    const struct mun_stackframe *stack[16];
};

// Return the last thrown error. There's no way to mark an error as "swallowed",
// so this information may be outdated. Only valid until the next call to `mun_error_at`.
struct mun_error *mun_last_error(void);

// The code of the last error.
#define mun_errno (mun_last_error()->code)

// Make `mun_last_error` point to a different location for this thread; return the old one.
struct mun_error *mun_set_error_storage(struct mun_error *);

// Overwrite the last error with a new one, with a `printf`-style message. Always "fails".
int mun_error_at(int, const char *name, const struct mun_stackframe *, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

// Add a stack frame to the last error, if there's space for it. Always "fails".
int mun_error_up(const struct mun_stackframe *);

// Add a stack frame to the last error and a prefix to the text. Always "fails".
int mun_error_up_ctx(const struct mun_stackframe *, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Print an error to stderr, possibly with pretty colored highlighting. If `err` is NULL,
// `mun_last_error()` is used. The first line looks something like "{prefix} error {code}:
// {message}", so, for example, `mun_error_show("oh no, it's the", NULL)` will display
// "oh no, it's the error 12345: something truly awful happened!"
void mun_error_show(const char *prefix, const struct mun_error *err);

// `struct mun_stackframe` desribing the position where this macro was used.
#define MUN_CURRENT_FRAME ({ static const struct mun_stackframe f = {__FILE__, __func__, __LINE__}; &f; })

// Call `mun_error_at` with the current stack frame, errno X, and name "X".
#define mun_error(id, ...) (mun_error_at(id, #id, MUN_CURRENT_FRAME, __VA_ARGS__), -1)

// Should be used as a suffix to an expression that returns something true-ish if it failed,
// in which case the current stack frame is marked in its error. Otherwise, 0 is returned.
#define MUN_RETHROW ? (mun_error_up(MUN_CURRENT_FRAME), -1) : 0

// Same as `MUN_RETHROW`, but adds a prefix to the text.
#define MUN_RETHROW_CTX(fmt, ...) ? (mun_error_up_ctx(MUN_CURRENT_FRAME, fmt, __VA_ARGS__), -1) : 0

// Same as `MUN_RETHROW`, but assumes the expression is a standard library function that
// sets `errno` and does not call `mun_error_at`.
#define MUN_RETHROW_OS ? (mun_error_at(-errno, "system", MUN_CURRENT_FRAME, "unknown"), -1) : 0

// Abort if the expression fails with a mun error, else return 0.
#define mun_cant_fail(e) (!(e) ? 0 : (mun_error_show("panic", NULL), abort(), -1))

// Abort if the expression is false, else return 0.
#define mun_assert(e, ...) mun_cant_fail((e) ? 0 : mun_error(EINVAL, __VA_ARGS__))

// Type-unsafe, but still pretty generic, dynamic vector. Zero-initialized, or with one
// of the `mun_vec_init_*` macros; finalized with `mun_vec_fini`.
//
//     struct vector_of_some_types mun_vec(some_type);
//     struct mun_vec(some_type) variable = {};
//     struct vector_of_some_types another_variable = {};
//
// Note: two `struct mun_vec(some_type)`s are technically not compatible with each other
//       when used in the same translation unit. They have the same layout, but they're
//       different types.
//
// Note: all of the below functions and macros take vectors by pointers.
//
#define mun_vec(T) { __typeof__(T)* data; size_t size, cap, off; }
#define mun_vec_type(v) __typeof__(*(v)->data)

// Weakly typed vector. Loses information about the contents, but allows any strongly
// typed vector to be passed to a function. (Type is `void` to make `sizeof` invalid.)
struct mun_vec mun_vec(void);

// Decay a strongly typed vector into a (sizeof(T), weakly typed vector) pair.
// Macros accept pointers to strongly typed vectors; functions with names ending with `_s`
// accept this pair of arguments instead.
#define mun_vec_strided(v) sizeof(mun_vec_type(v)), (struct mun_vec*)(v)
#define mun_vec_data_s(stride, v) ((char (*)[(stride)])((const struct mun_vec*){(v)})->data)

// Initializer for a vector that uses on-stack storage for `n` elements of type `T`.
// The resulting vector is empty, but can be appended to up to `n` times. More than that,
// and it will be reallocated on the heap.
//
//     struct mun_vec(char) xs = mun_vec_init_static(char, 128);
//
#define MUN_VEC_STATIC_BIT ((size_t)-1 ^ ((size_t)-1 >> 1))
#define mun_vec_init_static(T, n) {(T[n]){}, 0, (n) | MUN_VEC_STATIC_BIT, 0}

// Initializer for a vector that shares storage with some other pointer. It is assumed
// that the storage is pre-initialized, i.e. initial size is the same as capacity.
// The storage is not owned; if it was `malloc`ed, you're responsible for `free`ing it.
//
//     struct mun_vec(uint32_t) ys = mun_vec_init_borrow(calloc(4, 16), 16);
//     free(ys.data);
//
#define mun_vec_init_borrow(p, n) {p, n, (n) | MUN_VEC_STATIC_BIT, 0}

// Initializer for a vector that uses an array for underlying storage. DO NOT confuse
// arrays with pointers they decay to!
//
//     uint32_t zss[16] = {0};
//     struct mun_vec(uint32_t) zs = mun_vec_init_array(zss);
//     /* NOT VALID because sizeof(&zss[16]) == sizeof(void*) != sizeof(zss): */
//     struct mun_vec(uint32_t) ws = mun_vec_init_array(&zss[0]);
//
#define mun_vec_init_array(array) mun_vec_init_borrow(array, sizeof(array) / sizeof((array)[0]))

// Initializer for a vector that shares storage with a null-terminated string.
//
//     /* This vector should not be modified: the string is constant. */
//     struct mun_vec(char) rs = mun_vec_init_str("Hello, World!");
//
#define mun_vec_init_str(str) mun_vec_init_borrow(str, strlen(str))

// Finalizer of `struct mun_vec(T)`. The vector becomes empty (but still usable).
#define mun_vec_fini(v) mun_vec_fini_s(mun_vec_strided(v))

static inline void mun_vec_fini_s(size_t s, struct mun_vec *v) {
    if (v->cap & MUN_VEC_STATIC_BIT)
        v->size = 0;
    else
        free(mun_vec_data_s(s, v) - v->off), *v = (struct mun_vec){};
}

// Move the tail of a vector and change the size accordingly.
#define mun_vec_shift(v, start, offset) mun_vec_shift_s(mun_vec_strided(v), start, offset)

static inline void mun_vec_shift_s(size_t s, struct mun_vec *v, size_t start, int offset) {
    if (start < v->size)
        memmove(&mun_vec_data_s(s, v)[start + offset], &mun_vec_data_s(s, v)[start], (v->size - start) * s);
    v->size += offset;
}

// Resize the vector so that it may contain at least `n` elements. May fail with ENOMEM.
#define mun_vec_reserve(v, n) mun_vec_reserve_s(mun_vec_strided(v), n)

int mun_vec_reserve_s(size_t, struct mun_vec *, size_t n);

// Splice: insert `n` elements at `i`th position.
// Extend: insert `n` elements at the end.
// Insert: insert an element at `i`th position.
// Append: insert an element at the end.
#define mun_vec_splice(v, i, e, n) mun_vec_splice_s(mun_vec_strided(v), i, (const mun_vec_type(v)*){e}, n)
#define mun_vec_insert(v, i, e)    mun_vec_splice(v, i, e, 1)
#define mun_vec_extend(v, e, n)    mun_vec_extend_s(mun_vec_strided(v), (const mun_vec_type(v)*){e}, n)
#define mun_vec_append(v, e)       mun_vec_extend(v, e, 1)

static inline int mun_vec_splice_s(size_t s, struct mun_vec *v, size_t i, const void *e, size_t n) {
    if (!n)
        return 0;
    if (i == 0 && v->off >= n) {
        *v = (struct mun_vec){mun_vec_data_s(s, v) - n, v->size + n, v->cap + n, v->off - n};
    } else {
        if (mun_vec_reserve_s(s, v, v->size + n))
            return -1;
        mun_vec_shift_s(s, v, i, n);
    }
    memcpy(&mun_vec_data_s(s, v)[i], e, n * s);
    return 0;
}

static inline int mun_vec_extend_s(size_t s, struct mun_vec *v, const void *e, size_t n) {
    return mun_vec_splice_s(s, v, v->size, e, n); // not a macro to avoid evaluating `v` twice
}

// Remove `n` elements from the vector, starting with `i`th.
#define mun_vec_erase(v, i, n) mun_vec_erase_s(mun_vec_strided(v), i, n)

static inline void mun_vec_erase_s(size_t s, struct mun_vec *v, size_t i, size_t n) {
    if (i == 0)
        *v = (struct mun_vec){mun_vec_data_s(s, v) + n, v->size - n, v->cap - n, v->off + n};
    else
        mun_vec_shift_s(s, v, i + n, -(int)n);
}

// When used as `for mun_vec_iter(v, it) ...`, iterate over all elements of a vector `v`,
// storing pointers to each element as a new variable `it` in turn.
#define mun_vec_iter(v, it) (mun_vec_type(v) *it = (v)->data, *__end = &it[(v)->size]; it != __end; it++)

// Find the index of the first element that makes `expr` (where `_` is a pointer
// to that element) `true`; return the size of the vector if there is none.
#define mun_vec_find(vec, expr) ({                        \
    size_t __i = 0;                                       \
    for mun_vec_iter(vec, _) if (expr) break; else __i++; \
    __i;                                                  \
})

// Same as `mun_vec_find`, but faster and only works for vectors where `expr`
// is `false` for the first few elements and `true` for the rest.
#define mun_vec_bisect(vec, expr) ({                                \
    size_t __L = 0, __R = (vec)->size, __i;                         \
    while (__L != __R) {                                            \
        mun_vec_type(vec) *_ = &(vec)->data[__i = (__L + __R) / 2]; \
        if (expr) __R = __i; else __L = __i + 1;                    \
    }                                                               \
    __L;                                                            \
})

#if __cplusplus
} // extern "C"
#endif
