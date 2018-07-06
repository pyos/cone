#include <cxxabi.h>
#include <string.h>

extern "C" {

extern const size_t cone_cxa_globals_size = sizeof(__cxxabiv1::__cxa_eh_globals);

void cone_cxa_globals_save(void *s)
{
    memcpy(s, __cxxabiv1::__cxa_get_globals(), sizeof(__cxxabiv1::__cxa_eh_globals));
}

void cone_cxa_globals_load(void *s)
{
    memcpy(__cxxabiv1::__cxa_get_globals(), s, sizeof(__cxxabiv1::__cxa_eh_globals));
}

}
