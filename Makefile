CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += -Isrc $(shell $(PKG_CONFIG) --cflags gtk4 json-glib-1.0 gtksourceview-5)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -MMD -MP
LDLIBS += $(shell $(PKG_CONFIG) --libs gtk4 json-glib-1.0 gtksourceview-5) -lm
CORE = build/project.o build/document.o build/build_metadata.o build/graph_layout.o build/build.o build/session.o build/build_fixture.o build/workspace_tools.o build/extra_metadata.o build/language_client.o build/recovery.o build/snapshot.o
APP = $(CORE) build/app.o build/editor.o build/graph.o build/theme.o build/build_panel.o build/tools_ui.o build/language_ui.o build/main.o build/startapp-resources.o

.DELETE_ON_ERROR:

.PHONY: all run test test-ui clean check-deps
all: build/sourceatlas

check-deps:
	@$(PKG_CONFIG) --atleast-version=4.12 gtk4 || { printf '%s\n' 'GTK4 >= 4.12 development files are required.' >&2; exit 1; }
	@$(PKG_CONFIG) --atleast-version=1.6 json-glib-1.0 || { printf '%s\n' 'JSON-GLib >= 1.6 development files are required.' >&2; exit 1; }
	@$(PKG_CONFIG) --atleast-version=5.4 gtksourceview-5 || { printf '%s\n' 'GtkSourceView >= 5.4 development files are required.' >&2; exit 1; }

build:
	mkdir -p $@

build/%.o: src/%.c Makefile | build check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/sourceatlas: $(APP) Makefile
	$(CC) $(LDFLAGS) $(APP) $(LDLIBS) -o $@

build/startapp-resources.c: resources/startapp.gresource.xml resources/styles/startapp-dark.xml packaging/io.github.PaperGooseRus.StartApp.svg | build
	glib-compile-resources --generate-source --c-name startapp_resources --target $@ --sourcedir resources $<

build/startapp-resources.o: build/startapp-resources.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/test-core: tests/test_core.c $(CORE) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_core.c $(CORE) $(LDLIBS) -o $@

build/test-build: tests/test_build_run.c $(CORE) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_build_run.c $(CORE) $(LDLIBS) -o $@

build/test-tools: tests/test_tools.c $(CORE) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_tools.c $(CORE) $(LDLIBS) -o $@

build/test-workflows: tests/test_workflows.c $(CORE) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/test_workflows.c $(CORE) $(LDLIBS) -o $@

run: build/sourceatlas
	./build/sourceatlas .

test: build/test-core build/test-build build/test-tools build/test-workflows
	./build/test-core
	./build/test-build
	./build/test-tools
	./build/test-workflows

test-ui: build/sourceatlas
	sh tests/run_ui.sh

clean:
	rm -rf build

-include $(wildcard build/*.d)
