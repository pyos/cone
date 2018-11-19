.PHONY: clean tests tests/%
.PRECIOUS: obj/% obj/tests/%.o obj/tests/%

flags = -I. -Wall -Wextra -Wpointer-arith -fPIC -D_POSIX_C_SOURCE=200809L $(CFLAGS)

tests: tests/cone tests/perf

tests/%: obj/tests/%
	$<

obj/tests/%: obj/tests/%.o obj/libcxxcone.a
	$(CXX) -std=c++17 $(flags) $< -o $@ -Lobj -lcxxcone -ldl -pthread

obj/tests/%.o: tests/%.cc tests/base.cc cone.h cold.h mun.h cone.hh
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 $(flags) -DSRC=$< -c tests/base.cc -o $@

# This version is pure C:
obj/libcone.a: obj/cone.o obj/cold.o obj/mun.o
	ar rcs $@ $^

# This version requires linking with libcxxabi/libcxxrt/etc.:
obj/libcxxcone.a: obj/cone-cxa.o obj/cold.o obj/mun.o
	ar rcs $@ $^

obj/%.o: %.c cone.h cold.h mun.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(flags) -c $< -o $@

obj/cone-cxa.o: cone.c cone.h cold.h mun.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 $(flags) -DCONE_CXX=1 -c $< -o $@

clean:
	rm -rf obj
