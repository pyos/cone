CC     ?= gcc
CFLAGS ?= -O3

COMPILE = $(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_GNU_SOURCE -o
DYNLINK = $(CC) -shared -o
ARCHIVE = ar rcs


_require_headers = \
	libco/coro.h             \
	libco/events/fd.h        \
    libco/events/time.h      \
    libco/events/vec.h       \
	libco/evloop.h           \
	libco/generic/closure.h  \
	libco/generic/time.h     \
	libco/generic/u128.h     \
	libco/generic/vec.h


_require_objects = \
	obj/coro.o


_require_tests = \
	obj/test_simple      \
	obj/test_time_switch \
	obj/test_cno


.PHONY: all clean clean_all
.PRECIOUS: obj/%.o obj/test_%.o obj/test_%


all: $(_require_tests)

libcno/.git: .gitmodules
	git submodule update --init libcno

libcno/obj/libcno.a: libcno/.git
	$(MAKE) -C libcno obj/libcno.a

obj/%.o: libco/%.c $(_require_headers)
	@mkdir -p obj
	$(COMPILE) $@ $< -c

obj/test_%.o: tests/%.c libcno/.git $(_require_headers)
	@mkdir -p obj
	$(COMPILE) $@ $< -c -Ilibcno -Ilibco

obj/test_%: obj/test_%.o libcno/obj/libcno.a $(_require_objects)
	$(COMPILE) $@ $< $(_require_objects) -Llibcno/obj -ldl -lcno -pthread

clean:
	rm -rf obj

clean_all: clean
	$(MAKE) -C libcno clean
