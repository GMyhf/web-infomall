/*
 * verify.cpp — Archive integrity verification tool.
 *
 * Scans all shard index files (url_XX.idx) and data files,
 * verifying:
 *   - Shard structure (magic, entry_count, host_count, sizes)
 *   - Data file record structure (magic, sizes)
 *   - CRC-32 on every ArticleRecord where crc32 != 0
 *   - Cross-reference index entries against data file records
 *
 * Usage: ./verify <archive_dir>
 *   where archive_dir has data/ and index/ subdirectories.
 *
 * Build:
 *   clang++ -std=c++17 -O2 -c verify.cpp -o verify.o
 *   clang++ -std=c++17 -O2 -o verify verify.o query.o -lz -liconv -lpthread
 */

#include "common.h"
#include "query.h"   // MappedShard types (used implicitly for shard layout)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <cerrno>
#include <utility>
#include <set>

// ── Statistics ─────────────────────────────────────────────────

struct VerifyStats {
    uint64_t total_index_entries = 0;
    uint64_t total_data_records = 0;
    uint64_t records_with_crc32 = 0;
    uint64_t crc32_ok = 0;
    uint64_t crc32_mismatch = 0;
    uint64_t structural_errors = 0;
    uint64_t bad_entry_refs = 0;
    uint64_t good_entry_refs = 0;
    uint64_t unreferenced_data_records = 0;
    uint64_t shards_checked = 0;
    uint64_t shard_errors = 0;
    uint64_t data_files_scanned = 0;
    uint64_t missing_data_files = 0;

    void print() const {
        uint64_t total_issues = structural_errors + crc32_mismatch
                              + bad_entry_refs + unreferenced_data_records + shard_errors;

        printf("\n=== Verification Summary ===\n");
        printf("Shards checked:        %llu",
               (unsigned long long)shards_checked);
        if (shard_errors > 0)
            printf("  [%llu ERRORS]", (unsigned long long)shard_errors);
        printf("\n");
        printf("Index entries:         %llu\n",
               (unsigned long long)total_index_entries);
        printf("Data files scanned:    %llu\n",
               (unsigned long long)data_files_scanned);
        printf("Data records scanned:  %llu\n",
               (unsigned long long)total_data_records);
        printf("Records with CRC32:    %llu\n",
               (unsigned long long)records_with_crc32);
        printf("CRC32 matches:         %llu\n",
               (unsigned long long)crc32_ok);
        printf("CRC32 mismatches:      %llu\n",
               (unsigned long long)crc32_mismatch);
        printf("Cross-ref good:        %llu\n",
               (unsigned long long)good_entry_refs);
        printf("Cross-ref bad:         %llu\n",
               (unsigned long long)bad_entry_refs);
        printf("Unreferenced records:  %llu\n",
               (unsigned long long)unreferenced_data_records);
        printf("Missing data files:    %llu\n",
               (unsigned long long)missing_data_files);
        printf("Structural errors:     %llu\n",
               (unsigned long long)structural_errors);

        printf("\nResult: ");
        if (total_issues == 0) {
            printf("ALL CHECKS PASSED\n");
        } else {
            printf("%llu issue(s) found\n", (unsigned long long)total_issues);
        }
    }
};

// ── Filesystem Helpers ────────────────────────────────────────

static bool is_regular_file(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static bool is_directory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Discover all data_NNNN.dat files under archive/data/
// Returns sorted list of full paths.
static std::vector<std::string> discover_data_files(const std::string& data_dir) {
    std::vector<std::string> files;
    DIR* d = opendir(data_dir.c_str());
    if (!d) return files;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        // Skip non-directories; fall back to stat if d_type is unknown
        if (entry->d_type != DT_DIR) {
            if (entry->d_type == DT_UNKNOWN) {
                std::string candidate = data_dir + "/" + entry->d_name;
                if (!is_directory(candidate)) continue;
            } else {
                continue;
            }
        }
        // Expect 6-character YYYYMM directory names
        if (strlen(entry->d_name) != 6) continue;

        std::string month_path = data_dir + "/" + entry->d_name;
        DIR* md = opendir(month_path.c_str());
        if (!md) continue;

        struct dirent* fe;
        while ((fe = readdir(md)) != nullptr) {
            if (fe->d_name[0] == '.') continue;
            const char* name = fe->d_name;
            if (strncmp(name, "data_", 5) == 0) {
                const char* ext = strrchr(name, '.');
                if (ext && strcmp(ext, ".dat") == 0) {
                    files.push_back(month_path + "/" + name);
                }
            }
        }
        closedir(md);
    }
    closedir(d);

    std::sort(files.begin(), files.end());
    return files;
}

// Extract the YYYYMM directory name from a data file path.
//   /path/to/data/199901/data_0001.dat  ->  "199901"
static std::string month_from_data_path(const std::string& path) {
    // Find second-to-last path component
    size_t pos = path.rfind('/');
    if (pos == std::string::npos) return {};
    size_t prev = path.rfind('/', pos - 1);
    if (prev == std::string::npos) return {};
    return path.substr(prev + 1, 6);
}

static int sequence_from_data_path(const std::string& path) {
    size_t slash = path.rfind('/');
    const char* name = slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
    int sequence = 0;
    char trailing = 0;
    return sscanf(name, "data_%d.dat%c", &sequence, &trailing) == 1 && sequence > 0
        ? sequence : 0;
}

static std::string data_file_key(const std::string& month, uint32_t sequence) {
    return month + "/" + std::to_string(sequence == 0 ? 1 : sequence);
}

// ── Shard Structure Verification ───────────────────────────────

static bool verify_shard_structure(const std::string& path, VerifyStats& stats) {
    MappedShard shard;
    if (!shard.open(path.c_str())) {
        fprintf(stderr, "ERROR: Invalid or truncated shard %s\n", path.c_str());
        stats.shard_errors++;
        return false;
    }
    stats.shards_checked++;
    return true;
}

// ── Data Record Position Set (per data file) ──────────────────
//
// Stores enough record identity to prove an index entry points at the intended
// article, rather than merely at some record with the same offset and size.
// Used for O(log N) cross-referencing of index entries against
// data file records.
//
// UrlIndexEntry::reserved stores the sequence (legacy 0 means file 1), so
// collapsing all monthly files would allow a wrong sequence to pass by chance.

struct MonthRecordSet {
    struct RecordRef {
        uint32_t offset;
        uint32_t crawl_date;
        uint32_t mini_hash;
        uint16_t record_size;
        bool referenced = false;

        bool operator<(const RecordRef& other) const {
            if (offset != other.offset) return offset < other.offset;
            if (crawl_date != other.crawl_date) return crawl_date < other.crawl_date;
            if (mini_hash != other.mini_hash) return mini_hash < other.mini_hash;
            return record_size < other.record_size;
        }
        bool operator==(const RecordRef& other) const {
            return offset == other.offset && crawl_date == other.crawl_date &&
                   mini_hash == other.mini_hash && record_size == other.record_size;
        }
    };
    std::vector<RecordRef> records;

    void add(uint32_t offset, uint16_t size, uint32_t date, uint32_t hash) {
        records.push_back({offset, date, hash, size, false});
    }

    void sort_and_dedup() {
        std::sort(records.begin(), records.end());
        // Remove exact duplicates (same offset AND same size).
        // Keeps records with same offset but different sizes
        // (possible across multiple data files).
        auto last = std::unique(records.begin(), records.end());
        records.erase(last, records.end());
    }

    bool mark_match(uint32_t offset, uint16_t size, uint32_t date,
                    uint32_t hash) {
        auto it = std::lower_bound(records.begin(), records.end(), offset,
            [](const RecordRef& record, uint32_t wanted_offset) {
                return record.offset < wanted_offset;
            });
        while (it != records.end() && it->offset == offset) {
            if (it->record_size == size && it->crawl_date == date &&
                it->mini_hash == hash) {
                it->referenced = true;
                return true;
            }
            ++it;
        }
        return false;
    }
};

// ── Data File Scanning ────────────────────────────────────────

static void scan_data_file(const std::string& path,
                            VerifyStats& stats,
                            std::unordered_map<std::string, MonthRecordSet>& file_sets)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot open data file %s: %s\n",
                path.c_str(), strerror(errno));
        stats.structural_errors++;
        return;
    }

    // Get file size
    fseeko(f, 0, SEEK_END);
    off_t file_size = ftello(f);
    if (file_size == 0) {
        fclose(f);
        return;  // empty file, skip
    }
    rewind(f);

    stats.data_files_scanned++;
    std::string month = month_from_data_path(path);
    int sequence = sequence_from_data_path(path);
    if (month.size() != 6 || sequence <= 0) {
        fprintf(stderr, "ERROR: Invalid data file path %s\n", path.c_str());
        stats.structural_errors++;
        fclose(f);
        return;
    }
    MonthRecordSet& mset = file_sets[data_file_key(month, static_cast<uint32_t>(sequence))];

    // Reusable buffer for full-record reads
    std::vector<char> buf;
    off_t offset = 0;

    while (offset < file_size) {
        // Read the header first to check magic and sizes
        ArticleRecord hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
            fprintf(stderr, "ERROR: Truncated header at offset %lld in %s\n",
                    (long long)offset, path.c_str());
            stats.structural_errors++;
            break;
        }

        // Verify magic
        if (hdr.magic != ARTICLE_MAGIC) {
            fprintf(stderr, "ERROR: Bad magic 0x%08x at offset %lld in %s "
                    "(expected 0x%08x)\n",
                    hdr.magic, (long long)offset, path.c_str(), ARTICLE_MAGIC);
            stats.structural_errors++;
            break;
        }

        // Sanity-check record_size
        if (hdr.record_size < ArticleRecord::HEADER_SIZE || hdr.record_size > MAX_RECORD_SIZE) {
            fprintf(stderr, "ERROR: Record size %u < header size at offset %lld in %s\n",
                    hdr.record_size, (long long)offset, path.c_str());
            stats.structural_errors++;
            break;
        }

        if (offset > UINT32_MAX) {
            fprintf(stderr, "ERROR: Record offset %lld exceeds the 32-bit index range in %s\n",
                    (long long)offset, path.c_str());
            stats.structural_errors++;
            break;
        }

        uint64_t payload_size = static_cast<uint64_t>(ArticleRecord::HEADER_SIZE)
            + hdr.url_len + hdr.title_len + hdr.body_compr_len;
        if (payload_size != hdr.record_size) {
            fprintf(stderr, "ERROR: Record payload size %llu does not match record size %u "
                    "at offset %lld in %s\n", (unsigned long long)payload_size,
                    hdr.record_size, (long long)offset, path.c_str());
            stats.structural_errors++;
            break;
        }
        if (!valid_crawl_date(hdr.crawl_date)) {
            // Legacy index dates are still serviceable; a record header date is
            // not. Keep this separately visible so an operator does not mistake
            // a historical-date warning for a malformed record-size failure.
            fprintf(stderr, "WARNING: Record at offset %lld has invalid crawl date %u in %s\n",
                    (long long)offset, hdr.crawl_date, path.c_str());
            stats.structural_errors++;
            break;
        }

        if ((uint64_t)offset + hdr.record_size > (uint64_t)file_size) {
            fprintf(stderr, "ERROR: Record size %u exceeds file bounds at offset %lld in %s\n",
                    hdr.record_size, (long long)offset, path.c_str());
            stats.structural_errors++;
            break;
        }

        // Read enough of every record to verify the URL fingerprint. CRC-enabled
        // records already require the entire payload; legacy records only need
        // the URL bytes and can seek over the remainder.
        bool has_crc = (hdr.crc32 != 0);
        std::string record_url;

        if (has_crc) {
            // Read full record into buffer for CRC32 verification
            buf.resize(hdr.record_size);
            memcpy(buf.data(), &hdr, sizeof(hdr));

            size_t body_size = hdr.record_size - sizeof(hdr);
            if (body_size > 0 && fread(buf.data() + sizeof(hdr), 1, body_size, f) != body_size) {
                fprintf(stderr, "ERROR: Short read at offset %lld in %s\n",
                        (long long)offset, path.c_str());
                stats.structural_errors++;
                break;
            }

            // Verify CRC32
            stats.records_with_crc32++;
            uint32_t computed = compute_record_crc32(
                reinterpret_cast<const ArticleRecord*>(buf.data()));

            if (computed == hdr.crc32) {
                stats.crc32_ok++;
            } else {
                fprintf(stderr, "CRC32 MISMATCH at offset %lld in %s: "
                        "stored 0x%08x, computed 0x%08x\n",
                        (long long)offset, path.c_str(), hdr.crc32, computed);
                stats.crc32_mismatch++;
            }
            record_url.assign(buf.data() + ArticleRecord::HEADER_SIZE, hdr.url_len);
        } else {
            record_url.resize(hdr.url_len);
            if (hdr.url_len && fread(record_url.data(), 1, hdr.url_len, f) != hdr.url_len) {
                fprintf(stderr, "ERROR: Short URL read at offset %lld in %s\n",
                        (long long)offset, path.c_str());
                stats.structural_errors++;
                break;
            }
            uint32_t remaining = hdr.record_size - sizeof(hdr) - hdr.url_len;
            if (remaining > 0) {
                if (fseeko(f, remaining, SEEK_CUR) != 0) {
                    fprintf(stderr, "ERROR: Seek error at offset %lld in %s\n",
                            (long long)offset, path.c_str());
                    stats.structural_errors++;
                    break;
                }
            }
        }

        uint32_t actual_mini_hash = mini_hash(record_url);
        if (!valid_archive_url(record_url) || hdr.mini_hash != actual_mini_hash) {
            fprintf(stderr, "ERROR: Invalid URL fingerprint at offset %lld in %s\n",
                    (long long)offset, path.c_str());
            stats.structural_errors++;
        }
        mset.add(static_cast<uint32_t>(offset),
                 static_cast<uint16_t>(hdr.record_size), hdr.crawl_date,
                 actual_mini_hash);
        stats.total_data_records++;

        offset += hdr.record_size;
    }

    fclose(f);
}

// ── Cross-Reference Index Entries Against Data Records ────────

static void crossref_shard(const std::string& path,
                            VerifyStats& stats,
                            std::unordered_map<std::string, MonthRecordSet>& file_sets)
{
    MappedShard shard;
    if (!shard.open(path.c_str())) return;
    const ShardFileHeader* hdr = shard.header;
    const UrlIndexEntry* entries = shard.entries;

    stats.total_index_entries += hdr->entry_count;

    char month_buf[16];
    for (uint32_t i = 0; i < hdr->entry_count; i++) {
        const auto& e = entries[i];

        // Build YYYYMM from crawl_date
        if (!valid_crawl_date(e.crawl_date)) {
            fprintf(stderr, "WARNING: Entry [%u] has invalid crawl date %u in %s\n",
                    i, e.crawl_date, path.c_str());
            stats.bad_entry_refs++;
            continue;
        }
        snprintf(month_buf, sizeof(month_buf), "%04u%02u",
                 e.crawl_date / 10000, (e.crawl_date / 100) % 100);
        std::string month(month_buf, 6);
        uint32_t sequence = e.reserved == 0 ? 1 : e.reserved;
        std::string key = data_file_key(month, sequence);

        auto fit = file_sets.find(key);
        if (fit == file_sets.end()) {
            stats.missing_data_files++;
            fprintf(stderr, "WARNING: Entry offset=%u size=%u date=%u seq=%u "
                    "in %s: data file not found\n",
                    e.file_offset, e.record_size, e.crawl_date,
                    sequence, path.c_str());
            stats.bad_entry_refs++;
            continue;
        }

        uint32_t entry_mini_hash = static_cast<uint32_t>(e.url_hash) ^
                                   static_cast<uint32_t>(e.url_hash >> 32);
        bool index_url_ok = true;
        if (shard.is_v2) {
            std::string indexed_url = entry_url(e, shard.url_pool, hdr->url_pool_size);
            index_url_ok = valid_archive_url(indexed_url) &&
                           url_hash(indexed_url) == e.url_hash;
        }
        if (index_url_ok && fit->second.mark_match(
                e.file_offset, e.record_size, e.crawl_date, entry_mini_hash)) {
            stats.good_entry_refs++;
        } else {
            fprintf(stderr, "WARNING: Entry offset=%u size=%u date=%u in %s: "
                    "record identity mismatch in %s/data_%04u.dat\n",
                    e.file_offset, e.record_size, e.crawl_date,
                    path.c_str(), month.c_str(), sequence);
            stats.bad_entry_refs++;
        }
    }
}

// ── Main ──────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <archive_dir>\n", argv[0]);
        fprintf(stderr, "  Verifies integrity of archive at <archive_dir> which\n");
        fprintf(stderr, "  should contain data/ and index/ subdirectories.\n");
        return 1;
    }

    std::string archive_dir = argv[1];
    std::string data_dir = archive_dir + "/data";
    std::string index_dir = archive_dir + "/index";

    // Validate directory structure
    if (!is_directory(data_dir)) {
        fprintf(stderr, "ERROR: data/ directory not found at %s\n",
                data_dir.c_str());
        return 1;
    }
    if (!is_directory(index_dir)) {
        fprintf(stderr, "ERROR: index/ directory not found at %s\n",
                index_dir.c_str());
        return 1;
    }

    VerifyStats stats;

    printf("Web InfoMall Archive Integrity Verification\n");
    printf("Archive: %s\n", archive_dir.c_str());
    printf("Data:    %s\n", data_dir.c_str());
    printf("Index:   %s\n", index_dir.c_str());
    printf("\n");

    // ──────────────────────────────────────────────────────────
    // Phase 1: Shard Structure Verification
    // ──────────────────────────────────────────────────────────
    printf("=== Phase 1: Shard Structure Verification ===\n");

    int shards_found = 0;
    bool valid_shards[NUM_SHARDS] = {};
    for (int i = 0; i < NUM_SHARDS; i++) {
        char leaf[32];
        snprintf(leaf, sizeof(leaf), "/url_%02d.idx", i);
        std::string path = index_dir + leaf;
        if (!is_regular_file(path)) {
            continue;  // missing shard is normal (no entries for its hosts)
        }
        shards_found++;
        valid_shards[i] = verify_shard_structure(path, stats);
    }

    printf("  Found %d/%d shard files, %llu verified OK\n\n",
           shards_found, NUM_SHARDS,
           (unsigned long long)stats.shards_checked);

    if (shards_found == 0) {
        fprintf(stderr, "ERROR: No shard index files found in %s\n",
                index_dir.c_str());
        return 1;
    }

    // ──────────────────────────────────────────────────────────
    // Phase 2: Data File Scan — verify each record
    // ──────────────────────────────────────────────────────────
    printf("=== Phase 2: Data File Scan ===\n");

    auto data_files = discover_data_files(data_dir);
    printf("  Found %zu data files\n", data_files.size());

    std::unordered_map<std::string, MonthRecordSet> file_sets;
    for (const auto& df : data_files) {
        scan_data_file(df, stats, file_sets);
    }

    printf("  Files scanned:  %llu\n",
           (unsigned long long)stats.data_files_scanned);
    printf("  Records found:  %llu\n",
           (unsigned long long)stats.total_data_records);
    if (stats.records_with_crc32 > 0) {
        printf("  CRC32 checked:  %llu (%llu ok, %llu mismatches)\n",
               (unsigned long long)stats.records_with_crc32,
               (unsigned long long)stats.crc32_ok,
               (unsigned long long)stats.crc32_mismatch);
    } else {
        printf("  CRC32: none present (all crc32 fields are 0)\n");
    }

    // Sort month record sets for binary search
    std::set<std::string> months;
    for (auto& [key, mset] : file_sets) {
        mset.sort_and_dedup();
        months.insert(key.substr(0, 6));
    }
    printf("  Months covered: %zu\n\n", months.size());

    // ──────────────────────────────────────────────────────────
    // Phase 3: Cross-Reference — check every index entry
    // ──────────────────────────────────────────────────────────
    printf("=== Phase 3: Cross-Reference ===\n");

    for (int i = 0; i < NUM_SHARDS; i++) {
        if (!valid_shards[i]) continue;
        char leaf[32];
        snprintf(leaf, sizeof(leaf), "/url_%02d.idx", i);
        crossref_shard(index_dir + leaf, stats, file_sets);
    }

    for (const auto& [key, records] : file_sets) {
        (void)key;
        for (const auto& record : records.records) {
            if (!record.referenced) stats.unreferenced_data_records++;
        }
    }

    printf("  Entries checked: %llu\n",
           (unsigned long long)(stats.good_entry_refs + stats.bad_entry_refs));
    printf("  Good refs:       %llu\n",
           (unsigned long long)stats.good_entry_refs);
    printf("  Bad refs:        %llu\n",
           (unsigned long long)stats.bad_entry_refs);
    printf("  Unreferenced:    %llu\n",
           (unsigned long long)stats.unreferenced_data_records);

    // ──────────────────────────────────────────────────────────
    // Report
    // ──────────────────────────────────────────────────────────
    stats.print();

    return (stats.structural_errors > 0 ||
            stats.crc32_mismatch > 0 ||
            stats.bad_entry_refs > 0 ||
            stats.unreferenced_data_records > 0 ||
            stats.shard_errors > 0) ? 1 : 0;
}
