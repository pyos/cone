.PHONY: all clean

all: obj/test_cno obj/test_yield obj/test_simple

cno/.git: .gitmodules
	git submodule update --init cno
	touch cno/.git

cno/obj/libcno.a: cno/.git
	$(MAKE) -C cno obj/libcno.a

obj/test_cno: obj/cone.o obj/cold.o obj/tests/cno.o cno/obj/libcno.a
	$(CC) -Lcno/obj obj/cone.o obj/cold.o obj/tests/cno.o $(CFLAGS) -o obj/test_cno -ldl -lcno

obj/test_yield: obj/cone.o obj/cold.o obj/tests/yield.o
	$(CC) obj/cone.o obj/cold.o obj/tests/yield.o $(CFLAGS) -o obj/test_yield -ldl

obj/test_simple: obj/cone.o obj/cold.o obj/tests/simple.o
	$(CC) obj/cone.o obj/cold.o obj/tests/simple.o $(CFLAGS) -o obj/test_simple -ldl

obj/cone.o: cone.c cone.h
	@mkdir -p obj
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_GNU_SOURCE -c cone.c -o obj/cone.o

obj/cold.o: cold.c cone.h
	@mkdir -p obj
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_GNU_SOURCE -c cold.c -o obj/cold.o

obj/tests/cno.o: tests/cno.c cone.h cno/.git
	@mkdir -p obj/tests
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -Icno -D_GNU_SOURCE -c tests/cno.c -o obj/tests/cno.o

obj/tests/yield.o: tests/yield.c cone.h
	@mkdir -p obj/tests
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_GNU_SOURCE -c tests/yield.c -o obj/tests/yield.o

obj/tests/simple.o: tests/simple.c cone.h
	@mkdir -p obj/tests
	$(CC) -std=c11 -I. -Wall -Wextra -fPIC $(CFLAGS) -D_GNU_SOURCE -c tests/simple.c -o obj/tests/simple.o

clean:
	rm -rf obj
