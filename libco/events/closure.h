#pragma once
#include "../generic/time.h"

struct co_closure
{
    int (*function)(void*);
    void *data;
};

struct co_call_at
{
    struct co_closure cb;
    struct co_nsec time;
};

#define co_bind(f, data) ((struct co_closure){(int(*)(void*))f, data})
