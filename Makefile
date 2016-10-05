.PHONY: all clean tests/%
.PRECIOUS: obj/%.o obj/tests/%

all: obj/test_cno obj/test_yield obj/test_simple obj/test_romp

cno/.git: .gitmodules
	git submodule update --init cno
	touch cno/.git

cno/obj/libcno.a: cno/.git
	$(MAKE) -C cno obj/libcno.a

tests/%: obj/tests/%
	$<

obj/tests/%: tests/%.c tests/base.c obj/mun.o obj/cone.o obj/romp.o obj/cold.o cno/obj/libcno.a
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -Icno -D_GNU_SOURCE -DSRC=$< -Lcno/obj obj/mun.o obj/cone.o obj/romp.o obj/cold.o tests/base.c -o $@ -ldl -lcno

obj/%.o: %.c cone.h mun.h romp.h cno/.git
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -Icno -D_GNU_SOURCE -c $< -o $@

clean:
	rm -rf obj
