.PHONY: clean tests tests/%
.PRECIOUS: obj/%.o obj/tests/%

tests: tests/cone tests/romp tests/nero

tests/%: obj/tests/%
	$<

CCMD = $(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_POSIX_C_SOURCE=200809L

obj/tests/%: tests/%.c tests/base.c obj/mun.o obj/cone.o obj/cold.o obj/romp.o obj/nero.o
	@mkdir -p $(dir $@)
	$(CCMD) -DSRC=$< $(filter-out $<,$^) -o $@ -ldl

obj/%.o: %.c cone.h mun.h romp.h nero.h
	@mkdir -p $(dir $@)
	$(CCMD) -c $< -o $@

clean:
	rm -rf obj
