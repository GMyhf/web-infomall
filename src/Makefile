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

.PHONY: all clean test

all: load serve

# Data loading pipeline: parser + store + indexer (single TU for simplicity)
load: load.cpp common.h parser.cpp store.cpp indexer.cpp
	$(CXX) $(CXXFLAGS) -o load load.cpp $(LDFLAGS)

# Web replay server: query engine + HTTP server (single TU)
serve: server.cpp common.h query.cpp
	$(CXX) $(CXXFLAGS) -o serve server.cpp $(LDFLAGS)

# Standalone parser test
test_parse: test_parser.cpp common.h parser.cpp
	$(CXX) $(CXXFLAGS) -o test_parse test_parser.cpp $(LDFLAGS)

# Standalone store test
test_store: test_store.cpp common.h parser.cpp store.cpp
	$(CXX) $(CXXFLAGS) -o test_store test_store.cpp $(LDFLAGS)

test: test_parse
	@echo "=== Parser Test ==="
	./test_parse ../sample_data/dat0 10

clean:
	rm -f load serve test_parse test_store *.o *.dSYM

help:
	@echo "Targets:"
	@echo "  all         Build load and serve"
	@echo "  load        Build data loading pipeline"
	@echo "  serve       Build threaded HTTP replay server"
	@echo "  test_parse  Build parser test (standalone)"
	@echo "  test_store  Build store test (standalone)"
	@echo "  test        Run parser smoke test"
	@echo "  clean       Remove binaries"
