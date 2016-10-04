.PHONY: all clean
.PRECIOUS: obj/%.o

all: obj/test_cno obj/test_yield obj/test_simple

cno/.git: .gitmodules
	git submodule update --init cno
	touch cno/.git

cno/obj/libcno.a: cno/.git
	$(MAKE) -C cno obj/libcno.a

obj/test_%: obj/tests/%.o obj/cone.o obj/collld.o cno/obj/libcno.a
	$(CC) -Lcno/obj obj/cone.o obj/collld.o $< $(CFLAGS) -o $@ -ldl -lcno

obj/%.o: %.c cone.h cno/.git
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -Icno -D_GNU_SOURCE -c $< -o $@

clean:
	rm -rf obj
