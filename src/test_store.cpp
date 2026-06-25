/*
 * test_store.cpp — Standalone store/compress test.
 *
 * Usage: ./test_store <dat_file>
 */
#include "common.h"
#include "parser.h"
#include "store.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dat_file>\n", argv[0]);
        return 1;
    }
    DataStore store("test_archive");
    ArticleParser parser;

    int count = 0;
    parser.parse_file(argv[1], 1000, [&](auto& art) {
        uint32_t date = static_cast<uint32_t>(atoi(art.time.c_str()));
        auto loc = store.write_article(art.url, date, art.title, art.body);
        if (count % 100 == 0) {
            printf("  Wrote: %s offset=%lld size=%u\n",
                   loc.file_path.c_str(), loc.offset, loc.size);
        }
        count++;
    });
    printf("Stored %d articles\n", count);
    return 0;
}
