#pragma once
//
// mun // should've been in the standard library
//
// Okay, maybe these macros are a bit too weird, even for C.
//
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// A microsecond-resolution clock. That's good enough; epoll_wait(2) can't handle
// less than millisecond resolution anyway.
typedef int64_t mun_usec;

#define MUN_USEC_MAX INT64_MAX

mun_usec mun_usec_now(void);
mun_usec mun_usec_monotonic(void);

enum
{
    mun_errno_cancelled       = ECANCELED,
    mun_errno_assert          = EINVAL,
    mun_errno_memory          = ENOMEM,
    mun_errno_not_implemented = ENOSYS,
    // Define your own `mun_errno_X` values as `mun_errno_custom + N` to make `mun_error(X)` valid.
    mun_errno_custom          = 100000,
};

struct mun_stackframe
{
    const char *file;
    const char *func;
    unsigned line;
};

struct mun_error
{
    const char *name;
    // One of `mun_errno_*`, not necessarily from the above enum. Or `errno`
    // if this error was caused by a C library function.
    int code;
    // Number of entries in `stack` (from the beginning) that do not contain garbage.
    unsigned stacklen;
    char text[128];
    // Stack at the time of the error, starting from innermost frame (where `mun_error_at` was called).
    struct mun_stackframe stack[16];
};

// Return the last thrown error. Note that there's no way to mark an error as "swallowed",
// so this information may be outdated. Only valid until the next call to `mun_error_at`.
struct mun_error *mun_last_error(void);

// Overwrite the last error with a new one, with a brand new `printf`-style message.
int mun_error_at(int, const char *name, struct mun_stackframe, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

// Add a stack frame to the last error, if there's space for it.
int mun_error_up(struct mun_stackframe);

// Print an error to stderr, possibly with pretty colored highlighting. If `err` is NULL,
// `mun_last_error()` is used. The first line looks something like "{prefix} error {code}:
// {message}", so, for example, `mun_error_show("oh no, it's the", NULL)` will display
// "oh no, it's the error 12345: something truly awful happened!"
void mun_error_show(const char *prefix, const struct mun_error *err);

// `struct mun_stackframe` desribing the position where this macro was used.
#define MUN_CURRENT_FRAME ((struct mun_stackframe){__FILE__, __FUNCTION__, __LINE__})

// Call `mun_error_at` with the current stack frame, error id "mun_errno_X", and name "X".
#define mun_error(id, ...) mun_error_at(mun_errno_##id, #id, MUN_CURRENT_FRAME, __VA_ARGS__)

// When used as a suffix to an expression (e.g. `do_something MUN_RETHROW`), returns 0
// if the expression evaluates to `false` (returns 0, for example, meaning "no error"),
// else returns -1 and calls `mun_error_up` with the current frame. Intended to make
// stack traces point to correct line numbers (where the call actually happened, as opposed
// to some `if (ret != 0)` or `return mun_error_up()` later.)
#define MUN_RETHROW ? mun_error_up(MUN_CURRENT_FRAME) : 0

// Same as `MUN_RETHROW`, except assumes the expression does not use `mun_error`, but rather
// sets `errno` and converts the latter to the former.
#define MUN_RETHROW_OS ? mun_error_at(-errno, "errno", MUN_CURRENT_FRAME, "OS error") : 0

// Type-unsafe, but still pretty generic, dynamic vector.
//
// Initializer: zero.
// Finalizer: `mun_vec_fini`.
//
// Usage:
//     `struct mun_vec(some_type) variable = {};`
//     `struct vector_of_some_types mun_vec(some_type);`
//
// Note: two `struct mun_vec(some_type)`s are technically not compatible with each other
//       when used in the same translation unit. They have the same layout, but they're
//       different types.
//
// Note: all of the below functions and macros take vectors by pointers.
//
#define mun_vec(T) { T* data; unsigned size, cap; char is_static; }

// Weakly typed vector. Loses information about the contents, but allows any strongly
// typed vector to be passed to a function.
struct mun_vec mun_vec(char);

// "Return" the type of values stored in a vector.
#define mun_vec_type(v) __typeof__((v)->data[0])

// Decay a strongly typed vector into a (sizeof(T), weakly typed vector) pair.
// Macros accept pointers to strongly typed vectors; functions with names ending with `_s`
// accept this pair of arguments instead.
#define mun_vec_strided(v) sizeof(mun_vec_type(v)), (struct mun_vec*)(v)

// Initializer for a vector that uses on-stack storage for `n` elements of type `T`.
// The resulting vector is empty, but can be appended to up to `n` times.
//
//     struct mun_vec(char) xs = mun_vec_init_static(char, 128);
//
#define mun_vec_init_static(T, n) {(T[n]){}, 0, n, 1}

// Initializer for a vector that shares storage with some other pointer. It is assumed
// that the storage is pre-initialized, i.e. initial size is the same as capacity.
// The storage is not owned; if it was `malloc`ed, you're responsible for `free`ing it.
//
//     struct mun_vec(uint32_t) ys = mun_vec_init_borrow(calloc(4, 16), 16);
//     free(ys.data);
//
#define mun_vec_init_borrow(p, n) {p, n, n, 1}

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
//     /* Note that this vector should not be modified: the string is constant. */
//     struct mun_vec(char) rs = mun_vec_init_str("Hello, World!");
//
#define mun_vec_init_str(str) mun_vec_init_borrow(str, strlen(str))

// Finalizer of `struct mun_vec(T)`. The vector is still usable (and empty) after
// a call to this.
#define mun_vec_fini(v) mun_vec_fini_s((struct mun_vec *)(v))

static inline void mun_vec_fini_s(struct mun_vec *v) {
    if (!v->is_static)
        free(v->data), *v = (struct mun_vec){};
    else
        v->size = 0;
}

// Move the tail of a vector and change the size accordingly.
#define mun_vec_shift(v, start, offset) mun_vec_shift_s(mun_vec_strided(v), start, offset)

static inline void mun_vec_shift_s(size_t s, struct mun_vec *v, size_t start, int offset) {
    if (start < v->size)
        memmove(v->data + (start + offset) * s, v->data + start * s, (v->size - start) * s);
    v->size += offset;
}

// Resize the vector so that it may contain at least `n` more elements.
//
// Errors:
//     `memory`: ran out of heap space;
//     `memory`: this vector is static and cannot be resized.
//
#define mun_vec_reserve(v, n) mun_vec_reserve_s(mun_vec_strided(v), n)

static inline int mun_vec_reserve_s(size_t s, struct mun_vec *v, size_t n) {
    if (v->size + n <= v->cap)
        return 0;
    if (v->is_static)
        return mun_error(memory, "static vector of %u cannot fit %zu", v->cap, v->size + n);
    unsigned ncap = v->cap + (n > v->cap ? n : v->cap);
    void *r = realloc(v->data, ncap * s);
    if (r == NULL)
        return mun_error(memory, "%u * %zu bytes", ncap, s);
    v->data = r;
    v->cap = ncap;
    return 0;
}

// Splice: insert `n` elements at `i`th position.
// Extend: insert `n` elements at the end.
// Insert: insert an element at `i`th position.
// Append: insert an element at the end.
//
// Errors:
//     `memory`: see `mun_vec_reserve`.
//
#define mun_vec_splice(v, i, e, n) mun_vec_splice_s(mun_vec_strided(v), i, e, n)
#define mun_vec_insert(v, i, e)    mun_vec_splice(v, i, e, 1)
#define mun_vec_extend(v, e, n)    mun_vec_extend_s(mun_vec_strided(v), e, n)
#define mun_vec_append(v, e)       mun_vec_extend(v, e, 1)

static inline int mun_vec_splice_s(size_t s, struct mun_vec *v, size_t i, const void *e, size_t n) {
    if (!n)
        return 0;
    if (mun_vec_reserve_s(s, v, n))
        return -1;
    mun_vec_shift_s(s, v, i, n);
    memcpy(v->data + i * s, e, n * s);
    return 0;
}

static inline int mun_vec_extend_s(size_t s, struct mun_vec *v, const void *e, size_t n) {
    return mun_vec_splice_s(s, v, v->size, e, n);
}

// Remove `n` elements from the vector, starting with `i`th.
#define mun_vec_erase(v, i, n) mun_vec_erase_s(mun_vec_strided(v), i, n)

static inline void mun_vec_erase_s(size_t s, struct mun_vec *v, size_t i, size_t n) {
    mun_vec_shift_s(s, v, i + n, -(int)n);
}

// When used as `for mun_vec_iter(v, it) ...`, iterate over all elements of a vector `v`,
// storing pointers to each element as a new variable `it` in turn.
#define mun_vec_iter(v, it) (mun_vec_type(v) *it = (v)->data, *__end = &it[(v)->size]; it != __end; it++)

// Find the index of the first element that makes `expr` (where `_` is a pointer
// to that element) `true`; return the size of the vector if there is none.
#define mun_vec_find(vec, expr) ({                        \
    unsigned __i = 0;                                     \
    for mun_vec_iter(vec, _) if (expr) break; else __i++; \
    __i;                                                  \
})

// Same as `mun_vec_find`, but faster and only works for vectors where `expr`
// is `false` for the first few elements and `true` for the rest.
#define mun_vec_bisect(vec, expr) ({                                \
    unsigned __L = 0, __R = (vec)->size, __i;                       \
    while (__L != __R) {                                            \
        mun_vec_type(vec) *_ = &(vec)->data[__i = (__L + __R) / 2]; \
        if (expr) __R = __i; else __L = __i + 1;                    \
    }                                                               \
    __L;                                                            \
})

// A pair of objects stored by value.
//
// Initializer: designated.
// Finalizer: none.
//
#define mun_pair(A, B) { A a; B b; }
#define mun_pair_type_a(p) __typeof__((p)->a)
#define mun_pair_type_b(p) __typeof__((p)->b)

// Compute some unspecified non-cryptographic hash of the data in memory.
size_t mun_hash(const void *, size_t);

// A dense hash set. Values are hashed by their memory contents, or some prefix thereof,
// and compared byte-by-byte. `indices` is a sparse open-addressed set of indices into
// the dense `values` array; each index has the same hash as the value it points to.
// Supposedly, this improves locality.
//
// Initializer: zero.
// Finalizer: `mun_set_fini`.
//
#define mun_set(T)                    \
{                                     \
    unsigned pending_dels;            \
    struct mun_vec(unsigned) indices; \
    struct mun_vec(T) values;         \
}

struct mun_set mun_set(char);

// A dense hash map. Basically a set that stores (key, value) pairs and only computes
// the hash from the memory contents of the former.
//
// Initializer: zero.
// Finalizer: `mun_map_fini`.
//
#define mun_map(K, V) mun_set(struct mun_pair(K, V))

// "Return" the type of elements stored in the set.
#define mun_set_type(s) mun_vec_type(&(s)->values)
#define mun_map_type(m) mun_set_type(m)
#define mun_map_type_key(m) mun_pair_type_a((m)->values.data)

// Decay a strongly typed set into a (sizeof(key), sizeof(whole value), weakly typed set)
// triple.  For sets, sizeof(key) == sizeof(whole value); for maps, key is just the first
// element of the value pair.
#define mun_set_strided(s) sizeof(mun_set_type(s)), sizeof(mun_set_type(s)), (struct mun_set*)(s)
#define mun_map_strided(m) sizeof(mun_map_type_key(m)), sizeof(mun_map_type(m)), (struct mun_set*)(m)

// Finalizer of `struct mun_set(T)` and `struct mun_map(T)`. The set/map is empty
// and usable after a call to this.
#define mun_set_fini(s) mun_set_fini_s((struct mun_set*)(s))
#define mun_map_fini(m) mun_set_fini_s((struct mun_set*)(m))

static inline void mun_set_fini_s(struct mun_set *s) {
    mun_vec_fini(&s->indices);
    mun_vec_fini(&s->values);
    s->pending_dels = 0;
}

// Try to insert a new element into the set; return either a pointer to it, or to the same
// element that has been added earlier. For maps, the element is a key-value pair.
// `mun_map_insert3` copy-constructs one in-place from a key and a value.
//
// Errors:
//     `memory`: the set is overloaded, but there is not enough space to resize it.
//
#define mun_set_insert(s, v)     mun_set_insert_s(mun_set_strided(s), v)
#define mun_map_insert(m, p)     mun_set_insert_s(mun_map_strided(m), p)
#define mun_map_insert3(m, k, v) mun_map_insert(m, &((mun_map_type(m)){(k), (v)}))

static inline unsigned mun_set_index_s(size_t ks, size_t vs, struct mun_set *s, const void *k) {
    unsigned i = mun_hash(k, ks) & (s->indices.size - 1);
    while (s->indices.data[i] && (s->indices.data[i] == (unsigned)-1 || memcmp(k, &s->values.data[s->indices.data[i] * vs - vs], ks)))
        i = (i + 1) & (s->indices.size - 1);
    return i;
}

static inline void *mun_set_insert_nr(size_t ks, size_t vs, struct mun_set *s, const void *v) {
    unsigned i = mun_set_index_s(ks, vs, s, v);
    if (s->indices.data[i] == 0) {
        if (mun_vec_splice_s(vs, (struct mun_vec *)&s->values, s->values.size, v, 1) MUN_RETHROW)
            return NULL;
        s->indices.data[i] = s->values.size;
    }
    return &s->values.data[s->indices.data[i] * vs - vs];
}

static inline void *mun_set_insert_s(size_t ks, size_t vs, struct mun_set *s, const void *v) {
    unsigned load = s->values.size - s->pending_dels, size = s->indices.size;
    if (size == 0)
        size = 8;
    else if (load * 4 >= size * 3)
        size *= 2;
    else if (load * 4 <= size)
        size /= 2;
    if (size != s->indices.size || s->pending_dels * 4 > size) {
        struct mun_set q = {};
        if (mun_vec_reserve(&q.indices, size) || mun_vec_reserve_s(vs, (struct mun_vec *)&q.values, s->values.size - s->pending_dels))
            return mun_set_fini(&q), NULL;
        memset(q.indices.data, 0, (q.indices.size = size) * sizeof(unsigned));
        for mun_vec_iter(&s->indices, i)
            if (*i != 0 && *i != (unsigned)-1)
                mun_set_insert_nr(ks, vs, &q, &s->values.data[*i * vs - vs]);
        mun_set_fini(s);
        *s = q;
    }
    return mun_set_insert_nr(ks, vs, s, v);
}

// Check if the element pointed to by the return value of `mun_set_insert` was inserted
// by that call rather than some earlier one. Behavior is undefined if the set was
// modified in between the `mun_set_insert`/`mun_set_was_inserted` call pair.
#define mun_set_was_inserted(s, v) ((v) - (s)->values.data == (s)->values.size - 1)
#define mun_map_was_inserted(m, v) mun_set_was_inserted(m, v)

// Locate an element by its key. (For sets, the key is the value itself.) Return NULL
// if this value is not in the set.
#define mun_set_find(s, k) mun_set_find_s(mun_set_strided(s), k)
#define mun_map_find(m, k) mun_set_find_s(mun_map_strided(m), k)

static inline void *mun_set_find_s(size_t ks, size_t vs, struct mun_set *s, const void *k) {
    if (s->indices.size) {
        unsigned i = mun_set_index_s(ks, vs, s, k);
        if (s->indices.data[i])
            return &s->values.data[s->indices.data[i] * vs - vs];
    }
    return NULL;
}

// Locate a value by its key and remove it from the set. No-op if the element is not
// in the set.
#define mun_set_erase(s, k) mun_set_erase_s(mun_set_strided(s), k)
#define mun_map_erase(m, k) mun_set_erase_s(mun_map_strided(m), k)

static inline void mun_set_erase_s(size_t ks, size_t vs, struct mun_set *s, const void *k) {
    if (s->indices.size) {
        unsigned i = mun_set_index_s(ks, vs, s, k);
        if (s->indices.data[i])
            s->indices.data[i] = (unsigned)-1, s->pending_dels++;
    }
}
