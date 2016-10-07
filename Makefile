.PHONY: all clean tests/%
.PRECIOUS: obj/%.o obj/tests/%

all: tests/cone tests/romp tests/nero

tests/%: obj/tests/%
	$<

CCMD = $(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_POSIX_C_SOURCE=200809L

obj/tests/%: tests/%.c tests/base.c obj/mun.o obj/cone.o obj/romp.o obj/nero.o obj/cold.o
	@mkdir -p $(dir $@)
	$(CCMD) -DSRC=$< obj/mun.o obj/cone.o obj/romp.o obj/nero.o obj/cold.o tests/base.c -o $@ -ldl

obj/%.o: %.c cone.h mun.h romp.h nero.h cno/.git
	@mkdir -p $(dir $@)
	$(CCMD) -c $< -o $@

clean:
	rm -rf obj
