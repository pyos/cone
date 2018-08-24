.PHONY: clean tests tests/%
.PRECIOUS: obj/%.o obj/tests/%

tests: tests/cone

tests/%: obj/tests/%
	$<

CCMD = $(CC) -std=c11 -I. -Wall -Wextra -Wpointer-arith -fPIC $(CFLAGS) -D_POSIX_C_SOURCE=200809L

obj/tests/%: tests/%.c tests/base.c obj/libcone.a
	@mkdir -p $(dir $@)
	$(CCMD) -DSRC=$< tests/base.c -o $@ -Lobj -lcone -ldl

obj/libcone.a: obj/cone.o obj/cold.o obj/mun.o
	ar rcs $@ $^

obj/%.o: %.c cone.h mun.h
	@mkdir -p $(dir $@)
	$(CCMD) -c $< -o $@

clean:
	rm -rf obj
