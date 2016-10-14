#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    int code;
    unsigned stacklen;
    const char *name;
    char text[128];
    struct mun_stackframe stack[16];
};

struct mun_error *mun_last_error(void);
int  mun_error_at(int, const char *name, struct mun_stackframe, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
int  mun_error_up(struct mun_stackframe);
void mun_error_show(const char *prefix, const struct mun_error *err);

#define MUN_CURRENT_FRAME ((struct mun_stackframe){__FILE__, __FUNCTION__, __LINE__})
#define mun_error(id, ...) mun_error_at(mun_errno_##id, #id, MUN_CURRENT_FRAME, __VA_ARGS__)
#define MUN_RETHROW        ? mun_error_up(MUN_CURRENT_FRAME) : 0
#define MUN_RETHROW_OS     ? mun_error_at(-errno, "errno", MUN_CURRENT_FRAME, "OS error") : 0

#define mun_vec(T) { T* data; unsigned size, cap; char is_static; }
#define mun_vec_type(v)                 __typeof__((v)->data[0])
#define mun_vec_iter(v, it)             (mun_vec_type(v) *it = (v)->data, *__end = &it[(v)->size]; it != __end; it++)
#define mun_vec_strided(v)              sizeof(mun_vec_type(v)), (struct mun_vec*)(v)
#define mun_vec_init_static(T, n)       {(T[n]){}, 0, n, 1}
#define mun_vec_init_borrow(p, n)       {p,        n, n, 1}
#define mun_vec_init_array(array)       mun_vec_init_borrow(array, sizeof(array) / sizeof((array)[0]))
#define mun_vec_init_str(str)           mun_vec_init_borrow(str, strlen(str))
#define mun_vec_eq(a, b)                mun_vec_eq_s(mun_vec_strided(a), mun_vec_strided(b))
#define mun_vec_fini(v)                 mun_vec_fini_s((struct mun_vec *)(v))
#define mun_vec_shift(v, start, offset) mun_vec_shift_s(mun_vec_strided(v), start, offset)
#define mun_vec_reserve(v, elems)       mun_vec_reserve_s(mun_vec_strided(v), elems)
#define mun_vec_splice(v, i, elems, n)  mun_vec_splice_s(mun_vec_strided(v), i, elems, n)
#define mun_vec_extend(v, elems, n)     mun_vec_splice(v, (v)->size, elems, n)
#define mun_vec_insert(v, i, elem)      mun_vec_splice(v, i, elem, 1)
#define mun_vec_append(v, elem)         mun_vec_extend(v, elem, 1)
#define mun_vec_erase(v, i, n)          mun_vec_erase_s(mun_vec_strided(v), i, n)

struct mun_vec mun_vec(char);

static inline void mun_vec_fini_s(struct mun_vec *v) {
    if (!v->is_static)
        free(v->data), *v = (struct mun_vec){};
}

static inline int mun_vec_eq_s(size_t sa, const struct mun_vec *a, size_t sb, const struct mun_vec *b) {
    return sa == sb && a->size == b->size && !memcmp(a->data, b->data, a->size * sa);
}

static inline void mun_vec_shift_s(size_t s, struct mun_vec *v, size_t start, int offset) {
    if (start < v->size)
        memmove(v->data + (start + offset) * s, v->data + start * s, (v->size - start) * s);
    v->size += offset;
}

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

static inline int mun_vec_splice_s(size_t s, struct mun_vec *v, size_t i, const void *e, size_t n) {
    if (mun_vec_reserve_s(s, v, n))
        return -1;
    mun_vec_shift_s(s, v, i, n);
    memcpy(v->data + i * s, e, n * s);
    return 0;
}

static inline void mun_vec_erase_s(size_t s, struct mun_vec *v, size_t i, size_t n) {
    mun_vec_shift_s(s, v, i + n, -(int)n);
}

#define mun_vec_find(vec, eqexpr) ({                        \
    unsigned __i = 0;                                       \
    for mun_vec_iter(vec, _) if (eqexpr) break; else __i++; \
    __i;                                                    \
})

#define mun_vec_bisect(vec, ltexpr) ({                              \
    unsigned __L = 0, __R = (vec)->size, __M;                       \
    while (__L != __R) {                                            \
        mun_vec_type(vec) *_ = &(vec)->data[__M = (__L + __R) / 2]; \
        if (ltexpr) __R = __M; else __L = __M + 1;                  \
    }                                                               \
    __L;                                                            \
})

#define mun_set(T)                    \
{                                     \
    unsigned pending_dels;            \
    struct mun_vec(unsigned) indices; \
    struct mun_vec(T) values;         \
}

#define mun_set_type(s)            mun_vec_type(&(s)->values)
#define mun_set_strided(s)         sizeof(mun_set_type(s)), sizeof(mun_set_type(s)), (struct mun_set*)(s)
#define mun_set_fini(s)            mun_set_fini_s((struct mun_set*)(s))
#define mun_set_insert(s, v)       mun_set_insert_s(mun_set_strided(s), v)
#define mun_set_find(s, k)         mun_set_find_s(mun_set_strided(s), k)
#define mun_set_erase(s, k)        mun_set_erase_s(mun_set_strided(s), k)
#define mun_set_was_inserted(s, v) ((v) - (s)->values.data == (s)->values.size - 1)

#define mun_pair(A, B) { A a; B b; }
#define mun_pair_type_a(p)         __typeof__((p)->a)
#define mun_pair_type_b(p)         __typeof__((p)->b)
#define mun_map(K, V)              mun_set(struct mun_pair(K, V))
#define mun_map_type(m)            mun_set_type(m)
#define mun_map_type_key(m)        mun_pair_type_a((m)->values.data)
#define mun_map_strided(m)         sizeof(mun_map_type_key(m)), sizeof(mun_map_type(m)), (struct mun_set*)(m)
#define mun_map_fini(m)            mun_set_fini_s((struct mun_set*)(m))
#define mun_map_insert(m, p)       mun_set_insert_s(mun_map_strided(m), p)
#define mun_map_insert3(m, k, v)   mun_map_insert(m, &((mun_map_type(m)){k, v}))
#define mun_map_find(m, k)         mun_set_find_s(mun_map_strided(m), k)
#define mun_map_erase(m, k)        mun_set_erase_s(mun_map_strided(m), k)
#define mun_map_was_inserted(m, v) mun_set_was_inserted(m, v)

size_t mun_hash(const void *, size_t);

struct mun_set mun_set(char);

static inline void mun_set_fini_s(struct mun_set *s) {
    mun_vec_fini(&s->indices);
    mun_vec_fini(&s->values);
}

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

static inline void *mun_set_find_s(size_t ks, size_t vs, struct mun_set *s, const void *k) {
    if (s->indices.size) {
        unsigned i = mun_set_index_s(ks, vs, s, k);
        if (s->indices.data[i])
            return &s->values.data[s->indices.data[i] * vs - vs];
    }
    return NULL;
}

static inline void mun_set_erase_s(size_t ks, size_t vs, struct mun_set *s, const void *k) {
    if (s->indices.size) {
        unsigned i = mun_set_index_s(ks, vs, s, k);
        if (s->indices.data[i])
            s->indices.data[i] = (unsigned)-1, s->pending_dels++;
    }
}
