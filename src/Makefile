# Web InfoMall — C++ Archive System
#
# Build:  make          (builds load + serve)
# Test:   make test     (parser smoke test)
# Clean:  make clean

CXX = clang++
SDK_PATH = $(shell xcrun --show-sdk-path 2>/dev/null || echo /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk)
CXX_STDLIB = $(SDK_PATH)/usr/include/c++/v1
CXXFLAGS = -std=c++17 -O2 -Wall -cxx-isystem $(CXX_STDLIB)
LDFLAGS = -lz -liconv -lpthread

# Object files (one per .cpp with no main())
CORE_OBJS = parser.o store.o indexer.o query.o

.PHONY: all clean test

all: load serve verify bench

# Pattern rule: compile any .cpp to .o
%.o: %.cpp common.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Data loading pipeline
load: load.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) -o load load.o $(CORE_OBJS) $(LDFLAGS)

# Web replay server
serve: server.o query.o
	$(CXX) $(CXXFLAGS) -o serve server.o query.o $(LDFLAGS)

# Archive verify tool
verify: verify.o query.o
	$(CXX) $(CXXFLAGS) -o verify verify.o query.o $(LDFLAGS)

# Benchmark tool
bench: bench.o query.o
	$(CXX) $(CXXFLAGS) -o bench bench.o query.o $(LDFLAGS)

# Standalone parser test
test_parse: test_parser.o parser.o
	$(CXX) $(CXXFLAGS) -o test_parse test_parser.o parser.o $(LDFLAGS)

# Standalone store test
test_store: test_store.o parser.o store.o
	$(CXX) $(CXXFLAGS) -o test_store test_store.o parser.o store.o $(LDFLAGS)

test: test_parse
	@echo "=== Parser Test ==="
	./test_parse ../sample_data/dat0 10

clean:
	rm -f load serve verify bench test_parse test_store *.o *.dSYM

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
