#pragma once

struct co_callback
{
    int (*function)(void*);
    void *data;
};

#define co_callback_bind(f, data) __co_callback_bind((int(*)(void*))f, data)

static inline struct co_callback
__co_callback_bind(int (*f)(void*), void *data) {
    return (struct co_callback){f, data};
}
