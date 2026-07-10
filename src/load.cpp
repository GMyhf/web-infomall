/*
 * load.cpp — Main data loading pipeline.
 *
 * Orchestrates: Parser → Store → IndexBuilder
 *
 * Usage: ./load <dat_dir> <archive_dir> [--max N] [--files 0,1,2 | --all] [--incremental]
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
#include <dirent.h>
#include <cctype>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <climits>

static double elapsed() {
    static auto start = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start).count();
}

static std::vector<int> discover_data_files(const std::string& dat_dir) {
    std::vector<int> result;
    DIR* dir = opendir(dat_dir.c_str());
    if (!dir) return result;
    while (dirent* ent = readdir(dir)) {
        int index = -1;
        char trailing = 0;
        if (sscanf(ent->d_name, "dat%d%c", &index, &trailing) == 1 && index >= 0)
            result.push_back(index);
    }
    closedir(dir);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

static bool archive_has_data_files(const std::string& archive_dir) {
    std::string data_dir = archive_dir + "/data";
    DIR* months = opendir(data_dir.c_str());
    if (!months) return false;
    bool found = false;
    while (!found) {
        dirent* month = readdir(months);
        if (!month) break;
        if (month->d_name[0] == '.') continue;
        std::string month_path = data_dir + "/" + month->d_name;
        DIR* files = opendir(month_path.c_str());
        if (!files) continue;
        while (dirent* file = readdir(files)) {
            int sequence = 0;
            char trailing = 0;
            if (sscanf(file->d_name, "data_%d.dat%c", &sequence, &trailing) == 1 &&
                sequence > 0) {
                found = true;
                break;
            }
        }
        closedir(files);
    }
    closedir(months);
    return found;
}

constexpr uint32_t CHECKPOINT_MAGIC_V2 = 0x43503221; // "CP2!"
constexpr uint32_t CHECKPOINT_MAGIC_V3 = 0x43503321; // "CP3!"

struct LoadCheckpoint {
    std::set<int> completed;
    // A tainted source may already have records appended to the archive, but it
    // did not reach a fully published generation. Replaying it from byte zero
    // would duplicate those records, so only a full rebuild may clear this set.
    std::set<int> tainted;
};

class ArchiveLoadLock {
    int fd_ = -1;
public:
    bool acquire(const std::string& archive_dir) {
        fd_ = open((archive_dir + "/.load.lock").c_str(), O_CREAT | O_RDWR, 0644);
        return fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0;
    }
    ~ArchiveLoadLock() {
        if (fd_ >= 0) close(fd_);
    }
};

static bool read_checkpoint(const std::string& path, LoadCheckpoint& checkpoint) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    uint32_t first = 0;
    if (fread(&first, sizeof(first), 1, f) != 1) {
        fclose(f);
        return false;
    }
    LoadCheckpoint loaded;
    if (first == CHECKPOINT_MAGIC_V3) {
        uint32_t completed_count = 0;
        uint32_t tainted_count = 0;
        if (fread(&completed_count, sizeof(completed_count), 1, f) != 1 ||
            fread(&tainted_count, sizeof(tainted_count), 1, f) != 1 ||
            completed_count > 1000000 || tainted_count > 1000000 ||
            static_cast<uint64_t>(completed_count) + tainted_count > 1000000) {
            fclose(f);
            return false;
        }
        for (uint32_t i = 0; i < completed_count; i++) {
            int index = -1;
            if (fread(&index, sizeof(index), 1, f) != 1) {
                fclose(f);
                return false;
            }
            if (index >= 0) loaded.completed.insert(index);
        }
        for (uint32_t i = 0; i < tainted_count; i++) {
            int index = -1;
            if (fread(&index, sizeof(index), 1, f) != 1) {
                fclose(f);
                return false;
            }
            if (index >= 0) loaded.tainted.insert(index);
        }
        // A tainted marker is the conservative interpretation if a corrupt or
        // hand-written checkpoint lists the same source in both sets.
        for (int index : loaded.tainted) loaded.completed.erase(index);
    } else if (first == CHECKPOINT_MAGIC_V2) {
        uint32_t count = 0;
        if (fread(&count, sizeof(count), 1, f) != 1 || count > 1000000) {
            fclose(f);
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            int index = -1;
            if (fread(&index, sizeof(index), 1, f) != 1) {
                fclose(f);
                return false;
            }
            if (index >= 0) loaded.completed.insert(index);
        }
    } else {
        // Legacy checkpoint stored only the highest contiguous file index.
        int last = static_cast<int>(first);
        if (last < -1 || last > 1000000) {
            fclose(f);
            return false;
        }
        for (int i = 0; i <= last; i++) loaded.completed.insert(i);
    }
    bool exact = fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) exact = false;
    if (!exact) return false;
    checkpoint = std::move(loaded);
    return true;
}

static bool write_checkpoint(const std::string& path, const LoadCheckpoint& checkpoint) {
    if (checkpoint.completed.size() > 1000000 || checkpoint.tainted.size() > 1000000 ||
        checkpoint.completed.size() + checkpoint.tainted.size() > 1000000)
        return false;

    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    uint32_t completed_count = static_cast<uint32_t>(checkpoint.completed.size());
    uint32_t tainted_count = static_cast<uint32_t>(checkpoint.tainted.size());
    bool ok = fwrite(&CHECKPOINT_MAGIC_V3, sizeof(CHECKPOINT_MAGIC_V3), 1, f) == 1 &&
              fwrite(&completed_count, sizeof(completed_count), 1, f) == 1 &&
              fwrite(&tainted_count, sizeof(tainted_count), 1, f) == 1;
    for (int index : checkpoint.completed)
        ok = ok && fwrite(&index, sizeof(index), 1, f) == 1;
    for (int index : checkpoint.tainted)
        ok = ok && fwrite(&index, sizeof(index), 1, f) == 1;
    ok = ok && fflush(f) == 0 && fsync_file_descriptor(fileno(f));
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return fsync_parent_directory(path);
}

static bool finish_atomic_file(FILE* f, const std::string& tmp, const std::string& path,
                               bool ok) {
    ok = ok && fflush(f) == 0 && fsync_file_descriptor(fileno(f));
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return fsync_parent_directory(path);
}

static bool write_today(const std::string& index_dir,
                        const std::map<uint32_t, std::vector<std::string>>& today_urls) {
    std::string path = index_dir + "/today.dat";
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { fprintf(stderr, "ERROR: cannot write %s\n", tmp.c_str()); return false; }

    uint32_t num_days = static_cast<uint32_t>(today_urls.size());
    bool ok = fwrite(&num_days, sizeof(num_days), 1, f) == 1;
    for (auto& [mmdd, urls] : today_urls) {
        uint16_t m = static_cast<uint16_t>(mmdd);
        uint32_t cnt = static_cast<uint32_t>(urls.size());
        ok = ok && fwrite(&m, sizeof(m), 1, f) == 1 &&
             fwrite(&cnt, sizeof(cnt), 1, f) == 1;
        for (auto& url : urls) {
            uint16_t len = static_cast<uint16_t>(url.size());
            ok = ok && url.size() <= MAX_URL_LEN &&
                 fwrite(&len, sizeof(len), 1, f) == 1 &&
                 fwrite(url.data(), 1, len, f) == len;
        }
    }
    if (!finish_atomic_file(f, tmp, path, ok)) return false;
    printf("  Wrote today.dat (%u MMDD entries)\n", num_days);
    return true;
}

static bool write_year_dist(const std::string& index_dir,
                            const std::map<uint32_t, uint32_t>& year_counts) {
    std::string path = index_dir + "/year_dist.dat";
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { fprintf(stderr, "ERROR: cannot write %s\n", tmp.c_str()); return false; }

    uint32_t count = static_cast<uint32_t>(year_counts.size());
    bool ok = fwrite(&count, sizeof(count), 1, f) == 1;
    for (auto& [year, cnt] : year_counts) {
        uint32_t y = year, c = cnt;
        ok = ok && fwrite(&y, sizeof(y), 1, f) == 1 &&
             fwrite(&c, sizeof(c), 1, f) == 1;
    }
    if (!finish_atomic_file(f, tmp, path, ok)) return false;
    printf("  Wrote year_dist.dat (%u years)\n", count);
    return true;
}

static bool write_title_index(const std::string& index_dir,
                              const std::map<std::string, std::vector<TitlePosting>>& title_idx) {
    std::string path = index_dir + "/title_idx.dat";
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { fprintf(stderr, "ERROR: cannot write %s\n", tmp.c_str()); return false; }

    uint32_t num_terms = static_cast<uint32_t>(title_idx.size());
    bool ok = fwrite(&num_terms, sizeof(num_terms), 1, f) == 1;
    for (auto& [term, postings] : title_idx) {
        uint16_t tlen = static_cast<uint16_t>(term.size());
        ok = ok && term.size() <= UINT16_MAX &&
             fwrite(&tlen, sizeof(tlen), 1, f) == 1 &&
             fwrite(term.data(), 1, tlen, f) == tlen;
        // Cap at TOP_K
        uint32_t count = std::min<uint32_t>(postings.size(), TITLE_INDEX_TOP_K);
        ok = ok && fwrite(&count, sizeof(count), 1, f) == 1;
        for (uint32_t i = 0; i < count; i++) {
            auto& p = postings[i];
            uint16_t ulen = static_cast<uint16_t>(p.url.size());
            size_t title_size = std::min<size_t>(p.title.size(), UINT16_MAX);
            uint16_t tlen2 = static_cast<uint16_t>(title_size);
            ok = ok && p.url.size() <= MAX_URL_LEN && valid_crawl_date(p.date) &&
                 fwrite(&p.date, sizeof(p.date), 1, f) == 1 &&
                 fwrite(&ulen, sizeof(ulen), 1, f) == 1 &&
                 fwrite(p.url.data(), 1, ulen, f) == ulen &&
                 fwrite(&tlen2, sizeof(tlen2), 1, f) == 1 &&
                 fwrite(p.title.data(), 1, tlen2, f) == tlen2;
        }
    }
    if (!finish_atomic_file(f, tmp, path, ok)) return false;
    printf("  Wrote title_idx.dat (%u terms)\n", num_terms);
    return true;
}

// ── Incremental: read existing precomputed artifacts back into accumulators ──

static bool at_exact_eof(FILE* f) {
    int trailing = fgetc(f);
    return trailing == EOF && !ferror(f);
}

static uint64_t regular_file_size(FILE* f) {
    struct stat st;
    return fstat(fileno(f), &st) == 0 && st.st_size >= 0
        ? static_cast<uint64_t>(st.st_size) : 0;
}

static bool read_year_dist(const std::string& index_dir,
                           std::map<uint32_t, uint32_t>& year_counts) {
    const std::string path = index_dir + "/year_dist.dat";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    std::map<uint32_t, uint32_t> loaded;
    uint32_t count = 0;
    bool ok = fread(&count, sizeof(count), 1, f) == 1 && count <= 10000;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint32_t year = 0, articles = 0;
        ok = fread(&year, sizeof(year), 1, f) == 1 &&
             fread(&articles, sizeof(articles), 1, f) == 1 &&
             year > 0 && year <= 9999 && loaded.emplace(year, articles).second;
    }
    ok = ok && at_exact_eof(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "ERROR: Invalid or truncated auxiliary index %s\n", path.c_str());
        return false;
    }
    year_counts = std::move(loaded);
    return true;
}

static bool read_today(const std::string& index_dir,
                       std::map<uint32_t, std::vector<std::string>>& today_urls) {
    const std::string path = index_dir + "/today.dat";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    std::map<uint32_t, std::vector<std::string>> loaded;
    uint32_t num_days = 0;
    bool ok = fread(&num_days, sizeof(num_days), 1, f) == 1 && num_days <= 366;
    for (uint32_t i = 0; ok && i < num_days; i++) {
        uint16_t mmdd = 0;
        uint32_t url_count = 0;
        ok = fread(&mmdd, sizeof(mmdd), 1, f) == 1 &&
             fread(&url_count, sizeof(url_count), 1, f) == 1 &&
             valid_crawl_date(20000000u + mmdd) && url_count <= 200;
        std::vector<std::string> urls;
        if (ok) urls.reserve(url_count);
        for (uint32_t j = 0; ok && j < url_count; j++) {
            uint16_t len = 0;
            ok = fread(&len, sizeof(len), 1, f) == 1 && len > 0 && len <= MAX_URL_LEN;
            std::string url(len, '\0');
            ok = ok && fread(url.data(), 1, len, f) == len && valid_archive_url(url);
            if (ok) urls.push_back(std::move(url));
        }
        if (ok) ok = loaded.emplace(mmdd, std::move(urls)).second;
    }
    ok = ok && at_exact_eof(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "ERROR: Invalid or truncated auxiliary index %s\n", path.c_str());
        return false;
    }
    today_urls = std::move(loaded);
    return true;
}

static bool read_title_index(const std::string& index_dir,
                             std::map<std::string, std::vector<TitlePosting>>& title_idx) {
    constexpr uint32_t MAX_TITLE_TERMS = 10000000;
    const std::string path = index_dir + "/title_idx.dat";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    uint64_t file_size = regular_file_size(f);
    uint32_t num_terms = 0;
    bool ok = file_size >= sizeof(num_terms) &&
              fread(&num_terms, sizeof(num_terms), 1, f) == 1 &&
              num_terms <= MAX_TITLE_TERMS &&
              num_terms <= (file_size - sizeof(num_terms)) / 6;
    std::map<std::string, std::vector<TitlePosting>> loaded;
    for (uint32_t i = 0; ok && i < num_terms; i++) {
        uint16_t term_len = 0;
        ok = fread(&term_len, sizeof(term_len), 1, f) == 1 &&
             term_len > 0 && term_len <= 4096;
        std::string term(term_len, '\0');
        ok = ok && fread(term.data(), 1, term_len, f) == term_len;

        uint32_t posting_count = 0;
        ok = ok && fread(&posting_count, sizeof(posting_count), 1, f) == 1 &&
             posting_count <= static_cast<uint32_t>(TITLE_INDEX_TOP_K);
        std::vector<TitlePosting> postings;
        if (ok) postings.reserve(posting_count);
        for (uint32_t j = 0; ok && j < posting_count; j++) {
            TitlePosting posting;
            uint16_t url_len = 0, title_len = 0;
            ok = fread(&posting.date, sizeof(posting.date), 1, f) == 1 &&
                 valid_crawl_date(posting.date) &&
                 fread(&url_len, sizeof(url_len), 1, f) == 1 &&
                 url_len > 0 && url_len <= MAX_URL_LEN;
            posting.url.resize(url_len);
            ok = ok && fread(posting.url.data(), 1, url_len, f) == url_len &&
                 valid_archive_url(posting.url) &&
                 fread(&title_len, sizeof(title_len), 1, f) == 1;
            posting.title.resize(title_len);
            ok = ok && (title_len == 0 ||
                 fread(posting.title.data(), 1, title_len, f) == title_len);
            if (ok) postings.push_back(std::move(posting));
        }
        if (ok) ok = loaded.emplace(std::move(term), std::move(postings)).second;
    }
    ok = ok && at_exact_eof(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "ERROR: Invalid or truncated auxiliary index %s\n", path.c_str());
        return false;
    }
    title_idx = std::move(loaded);
    return true;
}

static bool read_archive_meta(const std::string& index_dir, size_t expected_articles,
                              ArchiveMeta& meta) {
    const std::string path = index_dir + "/meta.dat";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    ArchiveMeta loaded = {};
    bool ok = fread(&loaded, sizeof(loaded), 1, f) == 1 && at_exact_eof(f);
    if (fclose(f) != 0) ok = false;
    ok = ok && expected_articles <= UINT32_MAX &&
         loaded.total_articles == expected_articles &&
         loaded.total_urls <= loaded.total_articles;
    if (loaded.total_articles == 0) {
        ok = ok && loaded.total_urls == 0 && loaded.date_min == 0 && loaded.date_max == 0;
    } else {
        // total_urls == 0 is retained for compatibility with older metadata;
        // current writers always publish the real host count.
        ok = ok && valid_crawl_date(loaded.date_min) &&
             valid_crawl_date(loaded.date_max) && loaded.date_min <= loaded.date_max;
    }
    if (!ok) {
        fprintf(stderr, "ERROR: Invalid or inconsistent archive metadata %s\n", path.c_str());
        return false;
    }
    meta = loaded;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <dat_dir> <archive_dir> [--max N] "
            "[--files 0,1,2 | --all] [--incremental]\n",
            argv[0]);
        return 1;
    }

    std::string dat_dir = argv[1];
    std::string archive_dir = argv[2];

    DIR* source_dir = opendir(dat_dir.c_str());
    if (!source_dir) {
        fprintf(stderr, "ERROR: Cannot read source directory %s: %s\n",
                dat_dir.c_str(), strerror(errno));
        return 1;
    }
    closedir(source_dir);

    int max_articles = 0; // 0 = all
    std::vector<int> file_indices;
    bool use_all_files = false;
    bool incremental = false;
    bool selection_requested = false;
    bool explicit_files_requested = false;

    auto parse_nonnegative_int = [](const char* text, int& value) {
        if (!text || *text == '\0') return false;
        char* end = nullptr;
        errno = 0;
        long parsed = strtol(text, &end, 10);
        if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > INT_MAX)
            return false;
        value = static_cast<int>(parsed);
        return true;
    };

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--max") == 0) {
            if (i + 1 >= argc || !parse_nonnegative_int(argv[++i], max_articles)) {
                fprintf(stderr, "ERROR: --max requires a non-negative integer\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--all") == 0) {
            if (explicit_files_requested) {
                fprintf(stderr, "ERROR: --all and --files are mutually exclusive\n");
                return 1;
            }
            use_all_files = true;
            selection_requested = true;
        } else if (strcmp(argv[i], "--incremental") == 0) {
            incremental = true;
        } else if (strcmp(argv[i], "--files") == 0) {
            if (use_all_files || i + 1 >= argc) {
                fprintf(stderr, "ERROR: --files needs a comma-separated list and cannot follow --all\n");
                return 1;
            }
            selection_requested = true;
            explicit_files_requested = true;
            std::string list = argv[++i];
            size_t pos = 0;
            while (pos <= list.size()) {
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                std::string token = list.substr(pos, comma - pos);
                int file_index = -1;
                if (!parse_nonnegative_int(token.c_str(), file_index)) {
                    fprintf(stderr, "ERROR: Invalid --files item: %s\n", token.c_str());
                    return 1;
                }
                file_indices.push_back(file_index);
                if (comma == list.size()) break;
                pos = comma + 1;
            }
        } else {
            fprintf(stderr, "ERROR: Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (use_all_files) {
        file_indices = discover_data_files(dat_dir);
    } else if (file_indices.empty()) {
        file_indices = incremental && !selection_requested
            ? discover_data_files(dat_dir) : std::vector<int>{0};
    }

    std::sort(file_indices.begin(), file_indices.end());
    file_indices.erase(std::unique(file_indices.begin(), file_indices.end()), file_indices.end());

    if (file_indices.empty() && !incremental) {
        fprintf(stderr, "ERROR: No datN source files were found\n");
        return 1;
    }

    // Validate the complete selection before touching the archive. In
    // particular, an explicit/default missing source must not produce a
    // successful empty generation or a checkpoint that hides the omission.
    std::map<int, off_t> source_sizes;
    for (int file_index : file_indices) {
        const std::string path = dat_dir + "/dat" + std::to_string(file_index);
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "ERROR: Selected source file is missing or not regular: %s\n",
                    path.c_str());
            return 1;
        }
        source_sizes.emplace(file_index, st.st_size);
    }

    if (mkdir(archive_dir.c_str(), 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: Cannot create archive directory %s: %s\n",
                archive_dir.c_str(), strerror(errno));
        return 1;
    }
    ArchiveLoadLock load_lock;
    if (!load_lock.acquire(archive_dir)) {
        fprintf(stderr, "ERROR: Another loader is already using %s\n", archive_dir.c_str());
        return 1;
    }

    // Read checkpoint for incremental loading
    std::string index_dir = archive_dir + "/index";
    LoadCheckpoint checkpoint;
    size_t skipped_tainted = 0;
    if (!incremental) {
        bool existing_index = false;
        for (int sid = 0; sid < NUM_SHARDS && !existing_index; sid++) {
            char leaf[32];
            snprintf(leaf, sizeof(leaf), "/url_%02d.idx", sid);
            struct stat st;
            existing_index = stat((index_dir + leaf).c_str(), &st) == 0;
        }
        if (existing_index || archive_has_data_files(archive_dir)) {
            fprintf(stderr,
                "ERROR: Refusing a non-incremental load into a non-empty archive.\n"
                "       Use --incremental, or choose a new empty archive directory.\n");
            return 1;
        }
    }
    if (incremental) {
        std::string cp_path = index_dir + "/checkpoint.dat";
        bool have_checkpoint = read_checkpoint(cp_path, checkpoint);
        if (have_checkpoint) {
            printf("Checkpoint: %zu completed, %zu tainted source files\n",
                   checkpoint.completed.size(), checkpoint.tainted.size());
        } else {
            fprintf(stderr,
                "ERROR: Incremental loading requires a valid checkpoint. The loaded-source\n"
                "       baseline is unknown; rebuild into a new empty archive.\n");
            return 1;
        }
        std::vector<int> selected_tainted;
        for (int file_index : file_indices) {
            if (checkpoint.tainted.count(file_index) != 0)
                selected_tainted.push_back(file_index);
        }
        if (selection_requested && !selected_tainted.empty()) {
            fprintf(stderr,
                "ERROR: Refusing to replay tainted source file%s:",
                selected_tainted.size() == 1 ? "" : "s");
            for (int file_index : selected_tainted) fprintf(stderr, " dat%d", file_index);
            fprintf(stderr,
                "\n       These files may already be partially appended. Remove the archive\n"
                "       and perform a non-incremental full rebuild.\n");
            return 1;
        }
        if (!selection_requested) skipped_tainted = selected_tainted.size();

        // Never replay a completed or tainted source during automatic discovery.
        auto it = std::remove_if(file_indices.begin(), file_indices.end(),
            [&](int f) {
                return checkpoint.completed.count(f) != 0 ||
                       checkpoint.tainted.count(f) != 0;
            });
        file_indices.erase(it, file_indices.end());

        if (skipped_tainted > 0) {
            fprintf(stderr,
                "WARNING: Skipped %zu tainted source file(s); the archive contains a partial\n"
                "         import. A full rebuild is required to load those files completely.\n",
                skipped_tainted);
        }
    }

    if (file_indices.empty()) {
        if (skipped_tainted == 0) {
            printf("Archive is up to date; no source files need loading.\n");
        }
        return 0;
    }

    // Create archive directories
    std::string store_data_dir = archive_dir + "/data";
    if ((mkdir(store_data_dir.c_str(), 0755) != 0 && errno != EEXIST) ||
        (mkdir(index_dir.c_str(), 0755) != 0 && errno != EEXIST)) {
        fprintf(stderr, "ERROR: Cannot create archive subdirectories: %s\n", strerror(errno));
        return 1;
    }
    if (!fsync_directory_path(archive_dir)) {
        fprintf(stderr, "ERROR: Cannot sync archive directory %s: %s\n",
                archive_dir.c_str(), strerror(errno));
        return 1;
    }
    const std::string checkpoint_path = index_dir + "/checkpoint.dat";

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
    uint32_t date_min = UINT32_MAX, date_max = 0;
    uint32_t existing_articles = 0;
    std::set<int> completed_this_run;
    bool had_source_error = false;

    // Incremental merge: pull existing index + precomputed data into memory so
    // this run rewrites old + new together instead of overwriting the old.
    if (incremental) {
        printf("Incremental: merging with existing archive...\n");
        if (!indexer.load_existing()) {
            fprintf(stderr, "ERROR: Existing index is not safe to merge; archive was not changed\n");
            return 2;
        }
        if (!read_year_dist(index_dir, year_counts) ||
            !read_today(index_dir, today_urls) ||
            !read_title_index(index_dir, title_idx)) {
            fprintf(stderr,
                "ERROR: Existing auxiliary indexes are incomplete or invalid; incremental\n"
                "       loading was aborted before writing data. Perform a full rebuild.\n");
            return 2;
        }

        ArchiveMeta existing_meta = {};
        if (!read_archive_meta(index_dir, indexer.total_entries(), existing_meta)) {
            fprintf(stderr,
                "ERROR: Existing meta.dat does not match the primary index; incremental\n"
                "       loading was aborted before writing data. Perform a full rebuild.\n");
            return 2;
        }
        uint64_t distributed_articles = 0;
        for (const auto& [year, count] : year_counts) {
            (void)year;
            distributed_articles += count;
        }
        bool distribution_matches = distributed_articles == existing_meta.total_articles;
        if (existing_meta.total_articles > 0) {
            distribution_matches = distribution_matches && !year_counts.empty() &&
                year_counts.begin()->first == existing_meta.date_min / 10000 &&
                year_counts.rbegin()->first == existing_meta.date_max / 10000;
        }
        if (!distribution_matches) {
            fprintf(stderr,
                "ERROR: year_dist.dat counts do not match meta.dat; incremental loading\n"
                "       was aborted before writing data. Perform a full rebuild.\n");
            return 2;
        }
        existing_articles = existing_meta.total_articles;
        if (existing_meta.date_min < date_min) date_min = existing_meta.date_min;
        if (existing_meta.date_max > date_max) date_max = existing_meta.date_max;
        printf("  Carried over %u articles, %zu years, %zu MMDD, %zu title terms\n",
               existing_articles, year_counts.size(), today_urls.size(), title_idx.size());
    }
    double t0 = elapsed();

    for (int fidx : file_indices) {
        const std::string fname = dat_dir + "/dat" + std::to_string(fidx);
        printf("  Loading dat%d (%.0f MB)...", fidx, source_sizes.at(fidx) / 1048576.0);
        fflush(stdout);

        // Persist the non-replayable marker before the first append. If the
        // process exits anywhere below, the next incremental run cannot append
        // the same source prefix again from byte zero.
        checkpoint.completed.erase(fidx);
        checkpoint.tainted.insert(fidx);
        if (!write_checkpoint(checkpoint_path, checkpoint)) {
            fprintf(stderr, "ERROR: Cannot mark dat%d as tainted in %s\n",
                    fidx, checkpoint_path.c_str());
            return 2;
        }

        int file_count = 0;
        double t1 = elapsed();
        size_t errors_before = parser.errors().size();
        bool stopped_at_limit = false;
        bool validation_error = false;

        int requested = max_articles > 0 ? max_articles - total : 0;
        int parsed = parser.parse_file(fname.c_str(), requested,
            [&](auto& art) {
                if (art.time.size() != 8 ||
                    !std::all_of(art.time.begin(), art.time.end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
                    fprintf(stderr, "WARN: Invalid crawl date for article %d; skipping\n", art.id);
                    validation_error = true;
                    return;
                }
                uint32_t date = static_cast<uint32_t>(strtoul(art.time.c_str(), nullptr, 10));
                if (!valid_crawl_date(date) || !valid_archive_url(art.url)) {
                    fprintf(stderr, "WARN: Invalid date or URL for article %d; skipping\n", art.id);
                    validation_error = true;
                    return;
                }
                for (size_t i = 0; i < 5 && i < art.url.size(); i++)
                    art.url[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(art.url[i])));
                if (date < date_min) date_min = date;
                if (date > date_max) date_max = date;

                // Write to data store
                auto loc = store.write_article(art.url, date, art.title, art.body);
                if (!loc.ok) {
                    fprintf(stderr, "FATAL: Write failed at article %d, aborting\n", art.id);
                    exit(2);
                }

                // Add to index
                indexer.add_entry(art.url, date, loc.offset, loc.size,
                                  static_cast<uint32_t>(loc.file_seq));
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

        stopped_at_limit = max_articles > 0 && parsed >= requested;
        bool parse_error = parser.errors().size() != errors_before;
        bool source_error = parse_error || validation_error || parsed == 0 || file_count == 0;
        if (!stopped_at_limit && !source_error) {
            completed_this_run.insert(fidx);
        } else {
            if (source_error) had_source_error = true;
            const char* reason = stopped_at_limit ? "stopped at --max" :
                                 parse_error ? "encountered a read/parse error" :
                                 validation_error ? "contained invalid article fields" :
                                 "contained no valid articles";
            fprintf(stderr,
                "WARNING: dat%d is marked tainted because the import %s. It will not be\n"
                "         replayed; a full rebuild is required to load it completely.\n",
                fidx, reason);
        }

        total += file_count;
        double dt = elapsed() - t1;
        printf(" %d articles (%.1fs, %.0f rec/s)\n",
               file_count, dt, dt > 0 ? file_count / dt : 0);

        if (max_articles > 0 && total >= max_articles) break;
    }

    double load_time = elapsed() - t0;
    printf("\nLoaded %d articles in %.1fs\n", total, load_time);

    if (!store.finish()) {
        fprintf(stderr, "ERROR: Failed to flush archive data; index was not updated\n");
        return 2;
    }

    if (total == 0 && existing_articles == 0 && had_source_error) {
        fprintf(stderr, "ERROR: No valid articles were loaded; no index was published\n");
        return 2;
    }

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
    std::string meta_path = index_dir + "/meta.dat";
    std::string meta_tmp = meta_path + ".tmp";
    FILE* mf = fopen(meta_tmp.c_str(), "wb");
    ArchiveMeta meta = {};
    if (indexer.built_entries() > UINT32_MAX || indexer.total_hosts() > UINT32_MAX) {
        fprintf(stderr, "ERROR: Archive metadata counters exceed the v2 format\n");
        return 2;
    }
    meta.total_articles = static_cast<uint32_t>(indexer.built_entries());
    meta.total_urls = static_cast<uint32_t>(indexer.total_hosts());
    meta.date_min = date_min == UINT32_MAX ? 0 : date_min;
    meta.date_max = date_max;
    if (!mf || !finish_atomic_file(mf, meta_tmp, meta_path,
            fwrite(&meta, sizeof(meta), 1, mf) == 1)) {
        if (mf == nullptr) unlink(meta_tmp.c_str());
        fprintf(stderr, "ERROR: Cannot publish %s\n", meta_path.c_str());
        return 2;
    }
    printf("Meta: %u articles, %u hosts, date range %u - %u\n",
           meta.total_articles, meta.total_urls, meta.date_min, meta.date_max);

    // Write precomputed auxiliary data
    printf("\nWriting precomputed data...\n");
    if (!write_year_dist(index_dir, year_counts) || !write_today(index_dir, today_urls)) {
        fprintf(stderr, "ERROR: Failed to publish auxiliary indexes\n");
        return 2;
    }

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
    if (!write_title_index(index_dir, title_idx)) {
        fprintf(stderr, "ERROR: Failed to publish title index\n");
        return 2;
    }

    // Promote clean sources only after data, primary indexes, and auxiliary
    // indexes have all been published. Interrupted/error sources remain tainted.
    for (int file_index : completed_this_run) {
        checkpoint.tainted.erase(file_index);
        checkpoint.completed.insert(file_index);
    }
    if (!write_checkpoint(checkpoint_path, checkpoint)) {
        fprintf(stderr, "ERROR: Cannot write checkpoint %s\n", checkpoint_path.c_str());
        return 2;
    }
    printf("Checkpoint: %zu completed, %zu tainted source files\n",
           checkpoint.completed.size(), checkpoint.tainted.size());

    if (had_source_error) {
        fprintf(stderr,
                "ERROR: One or more source files were incomplete or invalid; "
                "the archive remains tainted\n");
        return 2;
    }

    printf("Done!\n");
    return 0;
}
