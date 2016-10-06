.PHONY: all clean tests/%
.PRECIOUS: obj/%.o obj/tests/%

all: tests/mun tests/cone tests/romp tests/nero

cno/.git: .gitmodules
	git submodule update --init cno
	touch cno/.git

cno/obj/libcno.a: cno/.git
	$(MAKE) -C cno obj/libcno.a

tests/%: obj/tests/%
	$<

CCMD = $(CC) -std=c11 -I. -Icno -Wall -Wextra -fPIC $(CFLAGS) -D_POSIX_C_SOURCE=200809L

obj/tests/%: tests/%.c tests/base.c obj/mun.o obj/cone.o obj/romp.o obj/nero.o obj/cold.o cno/obj/libcno.a
	@mkdir -p $(dir $@)
	$(CCMD) -DSRC=$< obj/mun.o obj/cone.o obj/romp.o obj/nero.o obj/cold.o tests/base.c -o $@ -ldl -Lcno/obj -lcno

obj/%.o: %.c cone.h mun.h romp.h nero.h cno/.git
	@mkdir -p $(dir $@)
	$(CCMD) -c $< -o $@

clean:
	rm -rf obj
