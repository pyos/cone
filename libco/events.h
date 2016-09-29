#pragma once
#ifndef COROUTINE_FD_BUCKETS
#define COROUTINE_FD_BUCKETS 127
#endif

#include "events/vec.h"
#include "events/time.h"
#if COROUTINE_EPOLL || (!defined(COROUTINE_EPOLL) && defined(__linux__))
#include "events/epoll.h"
#else
#include "events/select.h"
#endif
