CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += -Isrc $(shell $(PKG_CONFIG) --cflags gtk4 json-glib-1.0 gtksourceview-5)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -MMD -MP
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk4 json-glib-1.0 gtksourceview-5) -lm
CORE = build/project.o build/document.o build/build_metadata.o build/graph_layout.o build/build.o build/session.o build/build_fixture.o
APP = $(CORE) build/app.o build/editor.o build/graph.o build/theme.o build/build_panel.o build/main.o build/startapp-resources.o

.DELETE_ON_ERROR:

.PHONY: all run test test-ui clean check-deps
all: build/startapp

check-deps:
	@$(PKG_CONFIG) --atleast-version=4.12 gtk4 || { printf '%s\n' 'GTK4 >= 4.12 development files are required.' >&2; exit 1; }
	@$(PKG_CONFIG) --atleast-version=1.6 json-glib-1.0 || { printf '%s\n' 'JSON-GLib >= 1.6 development files are required.' >&2; exit 1; }
	@$(PKG_CONFIG) --atleast-version=5.4 gtksourceview-5 || { printf '%s\n' 'GtkSourceView >= 5.4 development files are required.' >&2; exit 1; }

build:
	mkdir -p $@

build/%.o: src/%.c Makefile | build check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/startapp: $(APP) Makefile
	$(CC) $(LDFLAGS) $(APP) $(LDLIBS) -o $@

build/startapp-resources.c: resources/startapp.gresource.xml resources/styles/startapp-dark.xml | build
	glib-compile-resources --generate-source --c-name startapp_resources --target $@ --sourcedir resources $<

build/startapp-resources.o: build/startapp-resources.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/test-core: tests/test_core.c $(CORE) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_core.c $(CORE) $(LDLIBS) -o $@

build/test-build: tests/test_build_run.c $(CORE) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_build_run.c $(CORE) $(LDLIBS) -o $@

run: build/startapp
	./build/startapp .

test: build/test-core build/test-build
	./build/test-core
	./build/test-build

test-ui: build/startapp
	sh tests/run_ui.sh

clean:
	rm -rf build

-include $(wildcard build/*.d)
