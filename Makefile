.PHONY: clean tests tests/%
.PRECIOUS: obj/%.o obj/tests/%

tests: tests/cone tests/siy tests/mae

tests/%: obj/tests/%
	$<

CCMD = $(CC) -std=c11 -I. -Wall -Wextra -Wpointer-arith -fPIC $(CFLAGS) -D_POSIX_C_SOURCE=200809L

obj/tests/%: tests/%.c tests/base.c obj/mun.o obj/cone.o obj/cold.o obj/siy.o obj/mae.o
	@mkdir -p $(dir $@)
	$(CCMD) -DSRC=$< $(filter-out $<,$^) -o $@ -ldl

obj/%.o: %.c cone.h mun.h siy.h mae.h
	@mkdir -p $(dir $@)
	$(CCMD) -c $< -o $@

clean:
	rm -rf obj
