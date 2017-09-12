.PHONY: clean tests tests/% tests/deck
.PRECIOUS: obj/%.o obj/tests/%

main: obj/main.o obj/libcone.a obj/deck.o
	$(CCMD) $^ -o $@ -ldl

tests: tests/cone tests/siy tests/mae tests/deck

tests/%: obj/tests/%
	$<

tests/deck: tests/deck.bash main
	bash tests/deck.bash 10 && /usr/bin/env python3 tests/deck.py

CCMD = $(CC) -std=c11 -I. -Wall -Wextra -Wpointer-arith -fPIC $(CFLAGS) -D_POSIX_C_SOURCE=200809L

obj/tests/%: tests/%.c tests/base.c obj/libcone.a
	@mkdir -p $(dir $@)
	$(CCMD) -DSRC=$< tests/base.c -o $@ -Lobj -lcone -ldl

obj/libcone.a: obj/cone.o obj/cold.o obj/mun.o obj/siy.o obj/mae.o
	ar rcs $@ $^

obj/%.o: %.c cone.h mun.h siy.h mae.h deck.h
	@mkdir -p $(dir $@)
	$(CCMD) -c $< -o $@

clean:
	rm -rf obj main
