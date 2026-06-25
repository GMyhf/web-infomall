/*
 * load.cpp — Main data loading pipeline.
 *
 * Orchestrates: Parser → Store → IndexBuilder
 *
 * Usage: ./load <dat_dir> <archive_dir> [--max N] [--files 0,1,2] [--incremental]
 */

#include "common.h"
#include "parser.h"
#include "store.h"
#include "indexer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <sys/stat.h>
#include <map>
#include <set>
#include <algorithm>

static double elapsed() {
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start).count();
}

static void write_today(const std::string& index_dir,
                         const std::map<uint32_t, std::vector<std::string>>& today_urls) {
    char path[256];
    snprintf(path, sizeof(path), "%s/today.dat", index_dir.c_str());
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "WARNING: cannot write %s\n", path); return; }

    uint32_t num_days = static_cast<uint32_t>(today_urls.size());
    fwrite(&num_days, sizeof(num_days), 1, f);
    for (auto& [mmdd, urls] : today_urls) {
        uint16_t m = static_cast<uint16_t>(mmdd);
        uint32_t cnt = static_cast<uint32_t>(urls.size());
        fwrite(&m, sizeof(m), 1, f);
        fwrite(&cnt, sizeof(cnt), 1, f);
        for (auto& url : urls) {
            uint16_t len = static_cast<uint16_t>(url.size());
            fwrite(&len, sizeof(len), 1, f);
            fwrite(url.data(), 1, url.size(), f);
        }
    }
    fclose(f);
    printf("  Wrote today.dat (%u MMDD entries)\n", num_days);
}

static void write_year_dist(const std::string& index_dir,
                             const std::map<uint32_t, uint32_t>& year_counts) {
    char path[256];
    snprintf(path, sizeof(path), "%s/year_dist.dat", index_dir.c_str());
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "WARNING: cannot write %s\n", path); return; }

    uint32_t count = static_cast<uint32_t>(year_counts.size());
    fwrite(&count, sizeof(count), 1, f);
    for (auto& [year, cnt] : year_counts) {
        uint32_t y = year, c = cnt;
        fwrite(&y, sizeof(y), 1, f);
        fwrite(&c, sizeof(c), 1, f);
    }
    fclose(f);
    printf("  Wrote year_dist.dat (%u years)\n", count);
}

static void write_title_index(const std::string& index_dir,
                               const std::map<std::string, std::vector<TitlePosting>>& title_idx) {
    char path[256];
    snprintf(path, sizeof(path), "%s/title_idx.dat", index_dir.c_str());
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "WARNING: cannot write %s\n", path); return; }

    uint32_t num_terms = static_cast<uint32_t>(title_idx.size());
    fwrite(&num_terms, sizeof(num_terms), 1, f);
    for (auto& [term, postings] : title_idx) {
        uint16_t tlen = static_cast<uint16_t>(term.size());
        fwrite(&tlen, sizeof(tlen), 1, f);
        fwrite(term.data(), 1, term.size(), f);
        // Cap at TOP_K
        uint32_t count = std::min<uint32_t>(postings.size(), TITLE_INDEX_TOP_K);
        fwrite(&count, sizeof(count), 1, f);
        for (uint32_t i = 0; i < count; i++) {
            auto& p = postings[i];
            uint16_t ulen = static_cast<uint16_t>(p.url.size());
            uint16_t tlen2 = static_cast<uint16_t>(p.title.size());
            fwrite(&p.date, sizeof(p.date), 1, f);
            fwrite(&ulen, sizeof(ulen), 1, f);
            fwrite(p.url.data(), 1, p.url.size(), f);
            fwrite(&tlen2, sizeof(tlen2), 1, f);
            fwrite(p.title.data(), 1, p.title.size(), f);
        }
    }
    fclose(f);
    printf("  Wrote title_idx.dat (%u terms)\n", num_terms);
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
    bool incremental = false;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            max_articles = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--all") == 0) {
            use_all_files = true;
        } else if (strcmp(argv[i], "--incremental") == 0) {
            incremental = true;
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

    // Read checkpoint for incremental loading
    std::string index_dir = archive_dir + "/index";
    int checkpoint_fidx = -1;
    if (incremental) {
        char cp_path[256];
        snprintf(cp_path, sizeof(cp_path), "%s/checkpoint.dat", index_dir.c_str());
        FILE* cf = fopen(cp_path, "rb");
        if (cf) {
            fread(&checkpoint_fidx, sizeof(checkpoint_fidx), 1, cf);
            fclose(cf);
            printf("Checkpoint: last file index = %d, skipping already-loaded files\n", checkpoint_fidx);
        } else {
            printf("No checkpoint found, starting from beginning\n");
        }
        // Skip already-loaded files
        auto it = std::remove_if(file_indices.begin(), file_indices.end(),
            [checkpoint_fidx](int f) { return f <= checkpoint_fidx; });
        file_indices.erase(it, file_indices.end());
    }

    // Create archive directories
    mkdir(archive_dir.c_str(), 0755);
    std::string store_data_dir = archive_dir + "/data";
    mkdir(store_data_dir.c_str(), 0755);
    mkdir(index_dir.c_str(), 0755);

    printf("Archive:  %s\n", archive_dir.c_str());
    printf("Data dir: %s\n", dat_dir.c_str());
    printf("Loading %zu file(s)\n\n", file_indices.size());

    DataStore store(archive_dir);
    Indexer indexer(index_dir);
    ArticleParser parser;

    // Accumulators for precomputed auxiliary data
    std::map<uint32_t, uint32_t> year_counts;     // year -> count
    std::map<uint32_t, std::vector<std::string>> today_urls; // mmdd -> URLs (up to 200)
    std::map<std::string, std::vector<TitlePosting>> title_idx; // term -> postings

    int total = 0;
    int last_fidx = -1;
    uint32_t date_min = UINT32_MAX, date_max = 0;
    double t0 = elapsed();

    for (int fidx : file_indices) {
        char fname[64];
        snprintf(fname, sizeof(fname), "%s/dat%d", dat_dir.c_str(), fidx);

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

                // Write to data store
                auto loc = store.write_article(art.url, date, art.title, art.body);
                if (!loc.ok) {
                    fprintf(stderr, "FATAL: Write failed at article %d, aborting\n", art.id);
                    exit(2);
                }

                // Add to index
                indexer.add_entry(art.url, date, loc.offset, loc.size);
                file_count++;

                // Accumulate year distribution
                year_counts[date / 10000]++;

                // Accumulate today-in-history (up to 200 unique URLs per MMDD)
                uint32_t mmdd = date % 10000;
                auto& today_list = today_urls[mmdd];
                if (today_list.size() < 200) {
                    // Dedup by linear scan (small list)
                    bool found = false;
                    for (auto& u : today_list) {
                        if (u == art.url) { found = true; break; }
                    }
                    if (!found) today_list.push_back(art.url);
                }

                // Accumulate title index
                if (!art.title.empty()) {
                    auto terms = tokenize_title(art.title);
                    for (auto& term : terms) {
                        auto& posts = title_idx[term];
                        if (posts.size() < TITLE_INDEX_TOP_K * 2) {
                            TitlePosting tp;
                            tp.url = art.url;
                            tp.date = date;
                            tp.title = art.title;
                            posts.push_back(std::move(tp));
                        }
                    }
                }
            });

        total += file_count;
        double dt = elapsed() - t1;
        printf(" %d articles (%.1fs, %.0f rec/s)\n",
               file_count, dt, dt > 0 ? file_count / dt : 0);

        if (max_articles > 0 && total >= max_articles) break;
        last_fidx = fidx;
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
        meta.total_urls = 0; // computed from host block count
        meta.date_min = date_min;
        meta.date_max = date_max;
        fwrite(&meta, sizeof(meta), 1, mf);
        fclose(mf);
        printf("Meta: %u articles, date range %u - %u\n", meta.total_articles, meta.date_min, meta.date_max);
    }

    // Write precomputed auxiliary data
    printf("\nWriting precomputed data...\n");
    write_year_dist(index_dir, year_counts);
    write_today(index_dir, today_urls);

    // Process and write title index (dedup, sort by date DESC, cap at TOP_K)
    printf("Processing title index (%zu terms)...\n", title_idx.size());
    for (auto& [term, posts] : title_idx) {
        // Sort by date DESC, then URL for determinism
        std::sort(posts.begin(), posts.end(),
            [](const TitlePosting& a, const TitlePosting& b) {
                if (a.date != b.date) return a.date > b.date;
                return a.url < b.url;
            });
        // Remove duplicates (same URL on same day)
        auto last = std::unique(posts.begin(), posts.end(),
            [](const TitlePosting& a, const TitlePosting& b) {
                return a.url == b.url && a.date == b.date;
            });
        posts.erase(last, posts.end());
        // Cap at TOP_K
        if (posts.size() > TITLE_INDEX_TOP_K)
            posts.resize(TITLE_INDEX_TOP_K);
    }
    write_title_index(index_dir, title_idx);

    // Write checkpoint for incremental loading
    if (last_fidx >= 0) {
        char cp_path[256];
        snprintf(cp_path, sizeof(cp_path), "%s/checkpoint.dat", index_dir.c_str());
        FILE* cf = fopen(cp_path, "wb");
        if (cf) {
            fwrite(&last_fidx, sizeof(last_fidx), 1, cf);
            fclose(cf);
            printf("Checkpoint: last file = dat%d\n", last_fidx);
        }
    }

    printf("Done!\n");
    return 0;
}
