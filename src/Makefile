# Web InfoMall — C++ Archive System
#
# Build: make
# Clean: make clean
# Test:  make test  (small test with 1000 articles)

CXX = clang++
CXXFLAGS = -std=c++17 -O2 -Wall
LDFLAGS = -lz -liconv

.PHONY: all clean test

all: load serve

# Loading pipeline: parser → store → indexer
load: load.cpp common.h parser.cpp store.cpp indexer.cpp
	$(CXX) $(CXXFLAGS) -o load load.cpp $(LDFLAGS)

# Web replay server: query engine + HTTP server
serve: server.cpp common.h query.cpp
	$(CXX) $(CXXFLAGS) -o serve server.cpp $(LDFLAGS)

# Quick test: parse 10 articles and print them
test_parse: parser.cpp common.h
	$(CXX) $(CXXFLAGS) -DPARSER_TEST -o test_parse parser.cpp $(LDFLAGS)

# Test store: parse + compress + write 1000 articles
test_store: store.cpp common.h parser.cpp
	$(CXX) $(CXXFLAGS) -DSTORE_TEST -o test_store store.cpp $(LDFLAGS)

test: test_parse
	@echo "=== Parser Test ==="
	./test_parse ../TenMillionArticles/dat/dat0 10

clean:
	rm -f load serve test_parse test_store *.o

# Helper targets
help:
	@echo "Targets:"
	@echo "  all         Build load and serve"
	@echo "  load        Build data loading pipeline"
	@echo "  serve       Build HTTP replay server"
	@echo "  test_parse  Build parser test (standalone)"
	@echo "  clean       Remove binaries"
