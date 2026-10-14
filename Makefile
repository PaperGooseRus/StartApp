CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += -Isrc $(shell $(PKG_CONFIG) --cflags gtk4)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -MMD -MP
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk4) -lm
CORE = build/project.o build/document.o
APP = $(CORE) build/app.o build/editor.o build/graph.o build/main.o

.DELETE_ON_ERROR:

.PHONY: all run test test-ui clean check-deps
all: build/startapp

check-deps:
	@$(PKG_CONFIG) --atleast-version=4.12 gtk4 || { printf '%s\n' 'GTK4 >= 4.12 development files are required.' >&2; exit 1; }

build:
	mkdir -p $@

build/%.o: src/%.c Makefile | build check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/startapp: $(APP) Makefile
	$(CC) $(LDFLAGS) $(APP) $(LDLIBS) -o $@

build/test-core: tests/test_core.c $(CORE) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_core.c $(CORE) $(LDLIBS) -o $@

run: build/startapp
	./build/startapp .

test: build/test-core
	./build/test-core

test-ui: build/startapp
	sh tests/run_ui.sh

clean:
	rm -rf build

-include $(wildcard build/*.d)
