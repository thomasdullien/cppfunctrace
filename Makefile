PREFIX      ?= /usr/local
CC          ?= cc
CFLAGS_BASE ?= -O2 -g -Wall -Wextra -Wno-missing-field-initializers
CFLAGS_LIB   = $(CFLAGS_BASE) -std=c11 -fPIC -fno-instrument-functions \
               -D_GNU_SOURCE -Iinclude -Isrc
CFLAGS_TOOL  = $(CFLAGS_BASE) -std=c11 -D_GNU_SOURCE -Isrc
LDFLAGS_LIB  = -shared -pthread -ldl

BUILD        = build
LIB          = $(BUILD)/libcppfunctrace.so
STATIC       = $(BUILD)/libcppfunctrace.a
FTRC2PF      = $(BUILD)/ftrc2perfetto

LIB_OBJS     = $(BUILD)/cppfunctrace.o $(BUILD)/intern.o
TOOL_OBJS    = $(BUILD)/ftrc2perfetto.o $(BUILD)/libftrc.o $(BUILD)/symresolve.o

.PHONY: all clean install test test-simple

all: $(LIB) $(STATIC) $(FTRC2PF)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS_LIB) -c -o $@ $<

$(LIB): $(LIB_OBJS)
	$(CC) $(LDFLAGS_LIB) -o $@ $^ -pthread -ldl

$(STATIC): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/ftrc2perfetto.o: src/ftrc2perfetto.c | $(BUILD)
	$(CC) $(CFLAGS_TOOL) -c -o $@ $<

$(BUILD)/libftrc.o: src/libftrc.c | $(BUILD)
	$(CC) $(CFLAGS_TOOL) -c -o $@ $<

$(BUILD)/symresolve.o: src/symresolve.c | $(BUILD)
	$(CC) $(CFLAGS_TOOL) -c -o $@ $<

$(FTRC2PF): $(TOOL_OBJS)
	$(CC) $(CFLAGS_TOOL) -o $@ $^ -ldl

install: all
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(STATIC) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 include/cppfunctrace.h $(DESTDIR)$(PREFIX)/include/
	install -m 755 $(FTRC2PF) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -rf $(BUILD)

# ── Tests ──────────────────────────────────────────────────────────

test: all test-simple test-threaded

$(BUILD)/test_simple: tests/test_simple.c $(LIB) | $(BUILD)
	$(CC) -O0 -g -finstrument-functions -rdynamic -o $@ tests/test_simple.c \
	    -L$(BUILD) -lcppfunctrace -Wl,-rpath,$(abspath $(BUILD))

$(BUILD)/test_threaded: tests/test_threaded.c $(LIB) | $(BUILD)
	$(CC) -O0 -g -finstrument-functions -rdynamic -pthread -o $@ tests/test_threaded.c \
	    -L$(BUILD) -lcppfunctrace -Wl,-rpath,$(abspath $(BUILD))

test-simple: $(BUILD)/test_simple $(FTRC2PF)
	@echo "== running test_simple =="
	@rm -rf $(BUILD)/traces-simple && mkdir -p $(BUILD)/traces-simple
	CPPFUNCTRACE_OUTPUT_DIR=$(BUILD)/traces-simple $(BUILD)/test_simple
	@ls -la $(BUILD)/traces-simple/
	$(FTRC2PF) -o $(BUILD)/traces-simple/out.perfetto-trace \
	    $(BUILD)/traces-simple/*.ftrc
	@ls -la $(BUILD)/traces-simple/out.perfetto-trace

test-threaded: $(BUILD)/test_threaded $(FTRC2PF)
	@echo "== running test_threaded =="
	@rm -rf $(BUILD)/traces-threaded && mkdir -p $(BUILD)/traces-threaded
	CPPFUNCTRACE_OUTPUT_DIR=$(BUILD)/traces-threaded $(BUILD)/test_threaded
	@ls -la $(BUILD)/traces-threaded/
	$(FTRC2PF) -o $(BUILD)/traces-threaded/out.perfetto-trace \
	    $(BUILD)/traces-threaded/*.ftrc
	@ls -la $(BUILD)/traces-threaded/out.perfetto-trace
