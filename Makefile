.PHONY: clean tests tests/%
.PRECIOUS: obj/%.o obj/tests/%.o obj/libcone.a obj/tests/%

flags = -I. -Wall -Wextra -Wpointer-arith -fPIC -D_POSIX_C_SOURCE=200809L $(CFLAGS)

tests: tests/cone tests/perf

tests/%: obj/tests/%
	$<

obj/tests/%: obj/tests/%.o obj/libcone.a
	$(CC) $< -o $@ -Lobj -lcone -ldl

obj/tests/%.o: tests/%.c tests/base.c cone.h mun.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(flags) -DSRC=$< -c tests/base.c -o $@

obj/libcone.a: obj/cone.o obj/cold.o obj/mun.o
	ar rcs $@ $^

obj/%.o: %.c cone.h mun.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(flags) -c $< -o $@

clean:
	rm -rf obj
