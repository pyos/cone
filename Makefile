.PHONY: clean tests tests/% tests/deck
.PRECIOUS: obj/%.o obj/tests/%

main: obj/main.o obj/mun.o obj/cone.o obj/cold.o obj/romp.o obj/nero.o obj/deck.o
	$(CCMD) $^ -o $@ -ldl

tests: tests/cone tests/romp tests/nero

tests/%: obj/tests/%
	$<

tests/deck: tests/deck.bash main
	bash tests/deck.bash 10 && python tests/deck.py

CCMD = $(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_POSIX_C_SOURCE=200809L

obj/tests/%: tests/%.c tests/base.c obj/mun.o obj/cone.o obj/cold.o obj/romp.o obj/nero.o obj/deck.o
	@mkdir -p $(dir $@)
	$(CCMD) -DSRC=$< $(filter-out $<,$^) -o $@ -ldl

obj/%.o: %.c cone.h mun.h romp.h nero.h deck.h
	@mkdir -p $(dir $@)
	$(CCMD) -c $< -o $@

clean:
	rm -rf obj main stress-test-data
