CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -fPIC

# The Lua version vis links (e.g. Arch's liblua.so.5.5), empty if unknown
VIS_LUA_VERSION ?= $(shell ldd "$$(command -v vis)" 2>/dev/null | grep -o 'lua[^ ]*\.so[^ ]*' | grep -o '5\.[0-9]' | head -1)

# Auto-detect the Lua headers matching vis; headers for another version crash vis
LUA_CFLAGS ?= $(shell \
	for p in lua5.5 lua-5.5 lua5.4 lua-5.4 lua lua5.3 lua-5.3 lua5.2 lua-5.2 lua5.1 luajit; do \
		pkg-config --exists "$$p" 2>/dev/null || continue; \
		case "$$(pkg-config --modversion "$$p")" in ("$(VIS_LUA_VERSION)"*) pkg-config --cflags "$$p"; exit 0;; esac; \
	done; \
	for d in /usr/include/lua5.4 /usr/include/lua5.3 /usr/include/lua5.2 /usr/include/lua5.1 /usr/local/include/lua5.4 /usr/local/include; do \
		[ -f "$$d/lua.h" ] && echo "-I$$d" && exit 0; \
	done)

TS_CFLAGS ?= $(shell pkg-config --cflags tree-sitter 2>/dev/null)
TS_LIBS ?= $(shell pkg-config --libs tree-sitter 2>/dev/null || echo -ltree-sitter)

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	SHARED_FLAGS = -bundle -undefined dynamic_lookup
	DL_LIBS =
else
	SHARED_FLAGS = -shared
	DL_LIBS = -ldl
endif

all: ts.so

ts.so: ts.c
	$(CC) $(CFLAGS) $(SHARED_FLAGS) $(LUA_CFLAGS) $(TS_CFLAGS) -o $@ $< $(TS_LIBS) $(DL_LIBS)

clean:
	rm -f ts.so

.PHONY: all clean
