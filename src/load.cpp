/*
 * load.cpp — Main data loading pipeline.
 *
 * Orchestrates: Parser → Store → IndexBuilder
 *
 * Usage: ./load <dat_dir> <archive_dir> [--max N] [--files 0,1,2]
 */

#include "common.h"
#include "parser.cpp"
#include "store.cpp"
#include "indexer.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>
#include <sys/stat.h>

static double elapsed() {
    static clock_t start = clock();
    return double(clock() - start) / CLOCKS_PER_SEC;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <dat_dir> <archive_dir> [--max N] [--files 0,1,2]\n", argv[0]);
        return 1;
    }

    std::string dat_dir = argv[1];
    std::string archive_dir = argv[2];

    int max_articles = 0; // 0 = all
    std::vector<int> file_indices;
    bool use_all_files = false;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max_articles = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--all") == 0) {
            use_all_files = true;
        } else if (strcmp(argv[i], "--files") == 0 && i + 1 < argc) {
            const char* p = argv[++i];
            while (*p) {
                file_indices.push_back(atoi(p));
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
    }

    if (use_all_files) {
        file_indices.clear();
        for (int i = 0; i < 112; i++) file_indices.push_back(i);
    } else if (file_indices.empty()) {
        file_indices = {0}; // Default: just dat0
    }

    // Create archive directories
    mkdir(archive_dir.c_str(), 0755);
    std::string store_data_dir = archive_dir + "/data";
    std::string index_dir = archive_dir + "/index";
    mkdir(store_data_dir.c_str(), 0755);
    mkdir(index_dir.c_str(), 0755);

    printf("Archive:  %s\n", archive_dir.c_str());
    printf("Data dir: %s\n", dat_dir.c_str());
    printf("Loading %zu file(s)\n\n", file_indices.size());

    DataStore store(archive_dir);
    Indexer indexer(index_dir);
    ArticleParser parser;

    int total = 0;
    uint32_t date_min = UINT32_MAX, date_max = 0;
    double t0 = elapsed();

    for (int fidx : file_indices) {
        char fname[64];
        snprintf(fname, sizeof(fname), "%s/dat%d", dat_dir.c_str(), fidx);

        // Check if file exists
        struct stat st;
        if (stat(fname, &st) != 0) {
            printf("  [SKIP] %s not found\n", fname);
            continue;
        }
        printf("  Loading dat%d (%.0f MB)...", fidx, st.st_size / 1048576.0);
        fflush(stdout);

        int file_count = 0;
        double t1 = elapsed();

        parser.parse_file(fname, (max_articles > 0 ? max_articles - total : 0),
            [&](auto& art) {
                uint32_t date = static_cast<uint32_t>(atoi(art.time.c_str()));
                if (date < date_min) date_min = date;
                if (date > date_max) date_max = date;
                auto loc = store.write_article(art.url, date, art.title, art.body);
                indexer.add_entry(art.url, date, loc.offset, loc.size);
                file_count++;
            });

        total += file_count;
        double dt = elapsed() - t1;
        printf(" %d articles (%.1fs, %.0f rec/s)\n",
               file_count, dt, dt > 0 ? file_count / dt : 0);

        if (max_articles > 0 && total >= max_articles) break;
    }

    double load_time = elapsed() - t0;
    printf("\nLoaded %d articles in %.1fs\n", total, load_time);

    // Build indices
    printf("\nBuilding index (%zu entries)...\n", indexer.total_entries());
    double t2 = elapsed();
    if (!indexer.build()) {
        fprintf(stderr, "ERROR: Index build failed\n");
        return 2;
    }
    double idx_time = elapsed() - t2;
    printf("Index built in %.1fs\n", idx_time);
    printf("\nTotal time: %.1fs\n", elapsed() - t0);

    // Write metadata
    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/meta.dat", index_dir.c_str());
    FILE* mf = fopen(meta_path, "wb");
    if (mf) {
        ArchiveMeta meta = {};
        meta.total_articles = total;
        meta.total_urls = 0; // computed from host block count (sum of host_count across shards)
        meta.date_min = date_min;
        meta.date_max = date_max;
        fwrite(&meta, sizeof(meta), 1, mf);
        fclose(mf);
        printf("Meta: %u articles, date range %u - %u\n", meta.total_articles, meta.date_min, meta.date_max);
    }

    printf("Done!\n");
    return 0;
}
