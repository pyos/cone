.PHONY: clean tests tests/%
.PRECIOUS: obj/% obj/tests/%.o obj/tests/%

flags = -I. -Wall -Wextra -Wpointer-arith -fPIC -D_POSIX_C_SOURCE=200809L $(CFLAGS)

tests: tests/cone tests/perf

tests/%: obj/tests/%
	$<

obj/tests/%: obj/tests/%.o obj/libcone.a
	$(CC) $< -o $@ -Lobj -lcone -ldl

obj/tests/%.o: tests/%.c tests/base.c cone.h mun.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(flags) -DSRC=$< -c tests/base.c -o $@

# This version is pure C:
obj/libcone.a: obj/cone.o obj/cold.o obj/mun.o
	ar rcs $@ $^

# This version requires linking with libcxxabi/libcxxrt/etc.:
obj/libcxxcone.a: obj/cone-cxa.o obj/cone-cc.o obj/cold.o obj/mun.o
	ar rcs $@ $^

obj/%.o: %.c cone.h mun.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(flags) -c $< -o $@

obj/cone-cxa.o: cone.c cone.h mun.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(flags) -DCONE_CXX=1 -c $< -o $@

obj/cone-cc.o: cone.cc cone.hh cone.h mun.h
	@mkdir -p $(dir $@)
	$(CXX) -std=c++14 $(flags) -DCONE_CXX=1 -c $< -o $@

clean:
	rm -rf obj
