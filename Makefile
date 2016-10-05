.PHONY: all clean tests/%
.PRECIOUS: obj/%.o obj/tests/%

all: tests/mun tests/cone tests/romp

tests/%: obj/tests/%
	$<

obj/tests/%: tests/%.c tests/base.c obj/mun.o obj/cone.o obj/romp.o obj/cold.o
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_GNU_SOURCE -DSRC=$< obj/mun.o obj/cone.o obj/romp.o obj/cold.o tests/base.c -o $@ -ldl

obj/%.o: %.c cone.h mun.h romp.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

clean:
	rm -rf obj
