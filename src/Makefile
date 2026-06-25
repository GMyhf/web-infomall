# Web InfoMall — C++ Archive System (cross-platform: Linux + macOS)
#
# Build:  make              (builds load + serve + verify + bench)
# Test:   make test         (parser smoke test)
# Clean:  make clean
#
# Toolchain is auto-detected from `uname -s`:
#   macOS  -> clang++ (+ SDK isysroot), links -lz -liconv
#   Linux  -> g++,     links -lz   (iconv is part of glibc)
#
# Overrides (pass on the make command line if your install is non-standard):
#   CXX=...            choose a different compiler
#   ZLIB_CFLAGS=-I...  extra include path for zlib.h
#   ZLIB_LIBS=...      how to link zlib (e.g. -l:libz.so.1 when no -dev symlink)
# Example (zlib header/lib in non-standard locations):
#   make ZLIB_CFLAGS=-I/opt/zlib/include ZLIB_LIBS="-L/opt/zlib/lib -lz"

UNAME_S := $(shell uname -s)

# ── Compiler (per-OS default, overridable) ────────────────────
ifeq ($(UNAME_S),Darwin)
  CXX ?= clang++
else
  CXX ?= g++
endif

CXXFLAGS ?= -std=c++17 -O2 -Wall

# ── zlib (override for non-standard installs) ─────────────────
# Auto-pick up a no-root install under ~/.local (e.g. zlib built from source),
# so `make` works even when /usr lacks the -dev package. Override to force.
ZLIB_CFLAGS ?=
ZLIB_LIBS   ?= -lz
ifneq ($(wildcard $(HOME)/.local/include/zlib.h),)
  ZLIB_CFLAGS += -I$(HOME)/.local/include
  ZLIB_LIBS   := -L$(HOME)/.local/lib $(ZLIB_LIBS)
endif

# ── Platform-specific flags ───────────────────────────────────
ifeq ($(UNAME_S),Darwin)
  # macOS: clang occasionally needs the SDK path to locate libc++ headers.
  SDK_PATH := $(shell xcrun --show-sdk-path 2>/dev/null)
  ifneq ($(SDK_PATH),)
    CXXFLAGS += -isysroot $(SDK_PATH)
  endif
  PLATFORM_LIBS := -liconv
else
  # Linux & others: iconv is in glibc, no separate lib needed.
  PLATFORM_LIBS :=
endif

CXXFLAGS += $(ZLIB_CFLAGS) -pthread
LDLIBS    = $(ZLIB_LIBS) $(PLATFORM_LIBS) -pthread
LDFLAGS  ?=

# Object files (one per .cpp with no main())
CORE_OBJS = parser.o store.o indexer.o query.o

.PHONY: all clean test help

all: load serve verify bench

# Pattern rule: compile any .cpp to .o
%.o: %.cpp common.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Data loading pipeline
load: load.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o load load.o $(CORE_OBJS) $(LDLIBS)

# Web replay server
serve: server.o query.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o serve server.o query.o $(LDLIBS)

# Archive verify tool
verify: verify.o query.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o verify verify.o query.o $(LDLIBS)

# Benchmark tool
bench: bench.o query.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o bench bench.o query.o $(LDLIBS)

# Standalone parser test
test_parse: test_parser.o parser.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o test_parse test_parser.o parser.o $(LDLIBS)

# Standalone store test
test_store: test_store.o parser.o store.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o test_store test_store.o parser.o store.o $(LDLIBS)

test: test_parse
	@echo "=== Parser Test ==="
	./test_parse ../sample_data/dat0 10

clean:
	rm -f load serve verify bench test_parse test_store *.o
	rm -rf *.dSYM

help:
	@echo "Targets:"
	@echo "  all         Build all binaries (load, serve, verify, bench)"
	@echo "  load        Build data loading pipeline"
	@echo "  serve       Build threaded HTTP replay server"
	@echo "  verify      Build archive integrity verification tool"
	@echo "  bench       Build query benchmark tool"
	@echo "  test_parse  Build parser test (standalone)"
	@echo "  test_store  Build store test (standalone)"
	@echo "  test        Run parser smoke test"
	@echo "  clean       Remove all binaries and object files"
	@echo ""
	@echo "Detected OS: $(UNAME_S);  CXX=$(CXX)"
