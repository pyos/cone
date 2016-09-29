#pragma once

struct co_callback
{
    int (*function)(void*);
    void *data;
};

struct co_event
{
    int (*connect)    (void*, struct co_callback);
    int (*disconnect) (void*, struct co_callback);
};

#define co_callback_bind(f, data) __co_callback_bind((int(*)(void*))f, data)

static inline struct co_callback
__co_callback_bind(int (*f)(void*), void *data) {
    return (struct co_callback){f, data};
}

#define co_event_impl(connect, disconnect) ((struct co_event) { \
    (int(*)(void*, struct co_callback)) connect,                \
    (int(*)(void*, struct co_callback)) disconnect,             \
})
