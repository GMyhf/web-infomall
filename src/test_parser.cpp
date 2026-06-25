/*
 * test_parser.cpp — Standalone parser smoke test.
 *
 * Usage: ./test_parse <dat_file> [max_articles]
 */
#include "common.h"
#include "parser.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dat_file> [max_articles]\n", argv[0]);
        return 1;
    }
    int max_articles = (argc > 2) ? atoi(argv[2]) : 100;
    ArticleParser parser;
    int count = parser.parse_file(argv[1], max_articles, [](auto& art) {
        printf("#%d %s | %.80s\n", art.id, art.time.c_str(), art.url.c_str());
        printf("  Title: %.60s\n", art.title.c_str());
        printf("  Body:  %.80s...\n\n", art.body.c_str());
    });
    printf("Parsed %d articles, %zu errors\n", count, parser.errors().size());
    return 0;
}
