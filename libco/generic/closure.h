#pragma once

struct co_closure
{
    int (*function)(void*);
    void *data;
};

#define co_bind(f, data) __co_bind((int(*)(void*))f, data)

static inline struct co_closure
__co_bind(int (*f)(void*), void *data) {
    return (struct co_closure){f, data};
}
