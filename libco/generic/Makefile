PYTHON ?= /usr/bin/env python3

GENERATE = $(PYTHON) __generate.py

.PHONY: all clean

all: vec~~call_at.h vec~~closure.h

clean:
	rm -f *~~*.h

vec~~call_at.h: vec~T~.h __generate.py
	$(GENERATE) $< T="struct co_call_at"

vec~~closure.h: vec~T~.h __generate.py
	$(GENERATE) $< T="struct co_closure"
