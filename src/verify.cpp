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
    uint64_t shards_checked = 0;
    uint64_t shard_errors = 0;
    uint64_t data_files_scanned = 0;
    uint64_t missing_data_files = 0;

    void print() const {
        uint64_t total_issues = structural_errors + crc32_mismatch
                              + bad_entry_refs + shard_errors;

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

// ── Shard Structure Verification ───────────────────────────────

static bool verify_shard_structure(const std::string& path, VerifyStats& stats) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n",
                path.c_str(), strerror(errno));
        stats.shard_errors++;
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "ERROR: Cannot stat %s: %s\n",
                path.c_str(), strerror(errno));
        close(fd);
        stats.shard_errors++;
        return false;
    }

    if (st.st_size < (off_t)sizeof(ShardFileHeader)) {
        fprintf(stderr, "ERROR: %s is too small (%lld bytes, need >= %zu)\n",
                path.c_str(), (long long)st.st_size, sizeof(ShardFileHeader));
        close(fd);
        stats.shard_errors++;
        return false;
    }

    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "ERROR: Cannot mmap %s: %s\n",
                path.c_str(), strerror(errno));
        close(fd);
        stats.shard_errors++;
        return false;
    }

    const auto* hdr = static_cast<const ShardFileHeader*>(data);
    bool ok = true;

    // Verify magic number
    bool is_v2 = (hdr->magic == SHARD_MAGIC);
    bool is_v1 = (hdr->magic == SHARD_MAGIC_V1);
    if (!is_v2 && !is_v1) {
        fprintf(stderr, "ERROR: %s has invalid magic 0x%08x "
                "(expected 0x%08x or 0x%08x)\n",
                path.c_str(), hdr->magic, SHARD_MAGIC, SHARD_MAGIC_V1);
        stats.shard_errors++;
        ok = false;
    }

    // Verify consistency of header fields
    uint64_t expected_size = sizeof(ShardFileHeader)
        + (uint64_t)hdr->host_count * sizeof(HostBlock)
        + (uint64_t)hdr->entry_count * sizeof(UrlIndexEntry)
        + (uint64_t)hdr->url_pool_size;

    if (expected_size != (uint64_t)st.st_size) {
        fprintf(stderr, "ERROR: %s size mismatch: header says %llu bytes, "
                "file is %lld bytes\n",
                path.c_str(),
                (unsigned long long)expected_size,
                (long long)st.st_size);
        stats.structural_errors++;
        ok = false;
    }

    // For v2, url_pool_size should be > 0 if entries exist
    if (is_v2 && hdr->entry_count > 0 && hdr->url_pool_size == 0) {
        fprintf(stderr, "WARNING: %s is v2 with %u entries but url_pool_size = 0\n",
                path.c_str(), hdr->entry_count);
        // This is unusual but not necessarily an error (URLs could all be empty)
    }

    if (ok) {
        stats.shards_checked++;
    }

    munmap(data, (size_t)st.st_size);
    close(fd);
    return ok;
}

// ── Data Record Position Set (per month) ──────────────────────
//
// Stores (offset, record_size) pairs for all data files in a month.
// Used for O(log N) cross-referencing of index entries against
// data file records.
//
// Because the index entry does not store the data file sequence
// number (only the YYYYMM from crawl_date), we collect offsets
// from all data files within a month. Duplicate offsets across
// files (extremely rare) are retained and searched linearly.

struct MonthRecordSet {
    std::vector<std::pair<uint32_t, uint16_t>> records;

    void add(uint32_t offset, uint16_t size) {
        records.emplace_back(offset, size);
    }

    void sort_and_dedup() {
        std::sort(records.begin(), records.end());
        // Remove exact duplicates (same offset AND same size).
        // Keeps records with same offset but different sizes
        // (possible across multiple data files).
        auto last = std::unique(records.begin(), records.end());
        records.erase(last, records.end());
    }

    // Check if this month has a record at the given offset with
    // the given record size.
    bool has_match(uint32_t offset, uint16_t size) const {
        auto it = std::lower_bound(records.begin(), records.end(),
                                   std::make_pair(offset, uint16_t(0)));
        while (it != records.end() && it->first == offset) {
            if (it->second == size) return true;
            ++it;
        }
        return false;
    }
};

// ── Data File Scanning ────────────────────────────────────────

static void scan_data_file(const std::string& path,
                            VerifyStats& stats,
                            std::unordered_map<std::string, MonthRecordSet>& month_sets)
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
    MonthRecordSet& mset = month_sets[month];

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
        if (hdr.record_size < ArticleRecord::HEADER_SIZE) {
            fprintf(stderr, "ERROR: Record size %u < header size at offset %lld in %s\n",
                    hdr.record_size, (long long)offset, path.c_str());
            stats.structural_errors++;
            break;
        }

        if ((uint64_t)offset + hdr.record_size > (uint64_t)file_size) {
            fprintf(stderr, "ERROR: Record size %u exceeds file bounds at offset %lld in %s\n",
                    hdr.record_size, (long long)offset, path.c_str());
            stats.structural_errors++;
            break;
        }

        // Record this position for cross-referencing
        mset.add(static_cast<uint32_t>(offset),
                 static_cast<uint16_t>(hdr.record_size));
        stats.total_data_records++;

        // Read the full record (or skip body if no CRC32)
        bool has_crc = (hdr.crc32 != 0);

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
        } else {
            // No CRC32 — skip past the body
            if (hdr.record_size > sizeof(hdr)) {
                if (fseeko(f, hdr.record_size - sizeof(hdr), SEEK_CUR) != 0) {
                    fprintf(stderr, "ERROR: Seek error at offset %lld in %s\n",
                            (long long)offset, path.c_str());
                    stats.structural_errors++;
                    break;
                }
            }
        }

        offset += hdr.record_size;
    }

    fclose(f);
}

// ── Cross-Reference Index Entries Against Data Records ────────

static void crossref_shard(const std::string& path,
                            const std::string& data_dir,
                            VerifyStats& stats,
                            const std::unordered_map<std::string, MonthRecordSet>& month_sets)
{
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "WARNING: Cannot open %s for cross-reference: %s\n",
                path.c_str(), strerror(errno));
        stats.shard_errors++;
        return;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        stats.shard_errors++;
        return;
    }

    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        stats.shard_errors++;
        return;
    }

    const auto* hdr = static_cast<const ShardFileHeader*>(data);
    const auto* hosts = reinterpret_cast<const HostBlock*>(hdr + 1);
    const auto* entries = reinterpret_cast<const UrlIndexEntry*>(hosts + hdr->host_count);

    stats.total_index_entries += hdr->entry_count;

    char month_buf[7];
    for (uint32_t i = 0; i < hdr->entry_count; i++) {
        const auto& e = entries[i];

        // Build YYYYMM from crawl_date
        snprintf(month_buf, sizeof(month_buf), "%04u%02u",
                 e.crawl_date / 10000, (e.crawl_date / 100) % 100);
        std::string month(month_buf, 6);

        auto mit = month_sets.find(month);
        if (mit == month_sets.end()) {
            // Check if month directory exists at all
            std::string month_dir = data_dir + "/" + month;
            if (is_directory(month_dir)) {
                // Month directory exists but has no records — possibly empty
                fprintf(stderr, "WARNING: Entry [%u] points to month %s "
                        "which has no scanned data records\n",
                        i, month.c_str());
            } else {
                // Month directory does not exist
                stats.missing_data_files++;
            }
            fprintf(stderr, "WARNING: Entry offset=%u size=%u date=%u "
                    "in %s: month %s not found\n",
                    e.file_offset, e.record_size, e.crawl_date,
                    path.c_str(), month.c_str());
            stats.bad_entry_refs++;
            continue;
        }

        if (mit->second.has_match(e.file_offset, e.record_size)) {
            stats.good_entry_refs++;
        } else {
            fprintf(stderr, "WARNING: Entry offset=%u size=%u in %s: "
                    "no matching record at offset in month %s\n",
                    e.file_offset, e.record_size,
                    path.c_str(), month.c_str());
            stats.bad_entry_refs++;
        }
    }

    munmap(data, (size_t)st.st_size);
    close(fd);
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
    for (int i = 0; i < NUM_SHARDS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/url_%02d.idx", index_dir.c_str(), i);
        if (!is_regular_file(path)) {
            continue;  // missing shard is normal (no entries for its hosts)
        }
        shards_found++;
        verify_shard_structure(path, stats);
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

    std::unordered_map<std::string, MonthRecordSet> month_sets;
    for (const auto& df : data_files) {
        scan_data_file(df, stats, month_sets);
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
    for (auto& [month, mset] : month_sets) {
        mset.sort_and_dedup();
    }
    printf("  Months covered: %zu\n\n", month_sets.size());

    // ──────────────────────────────────────────────────────────
    // Phase 3: Cross-Reference — check every index entry
    // ──────────────────────────────────────────────────────────
    printf("=== Phase 3: Cross-Reference ===\n");

    for (int i = 0; i < NUM_SHARDS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/url_%02d.idx", index_dir.c_str(), i);
        if (!is_regular_file(path)) continue;
        crossref_shard(path, data_dir, stats, month_sets);
    }

    printf("  Entries checked: %llu\n",
           (unsigned long long)(stats.good_entry_refs + stats.bad_entry_refs));
    printf("  Good refs:       %llu\n",
           (unsigned long long)stats.good_entry_refs);
    printf("  Bad refs:        %llu\n",
           (unsigned long long)stats.bad_entry_refs);

    // ──────────────────────────────────────────────────────────
    // Report
    // ──────────────────────────────────────────────────────────
    stats.print();

    return (stats.structural_errors > 0 ||
            stats.crc32_mismatch > 0 ||
            stats.bad_entry_refs > 0 ||
            stats.shard_errors > 0) ? 1 : 0;
}
