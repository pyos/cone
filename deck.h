#pragma once
#include "mun.h"
#include "cone.h"
#include "romp.h"
#include "nero.h"

struct deck
{
    uint32_t pid;
    uint32_t time;
    unsigned state;
    const char *name;
    struct cone_event wake;
    struct mun_vec(char) fname_request;
    struct mun_vec(char) fname_release;
    struct mun_vec(struct deck_nero) rpcs;
    struct mun_vec(struct deck_request) queue;
};

void deck_fini    (struct deck *);
int  deck_add     (struct deck *, struct nero *);
void deck_del     (struct deck *, struct nero *);
int  deck_acquire (struct deck *);
int  deck_release (struct deck *);
int  deck_acquired(struct deck *d);
