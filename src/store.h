/*
 * store.h — Append-only ArticleRecord data file writer.
 */

#ifndef STORE_H
#define STORE_H

#include "common.h"
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>

// ── DataFile Writer ───────────────────────────────────────────

class DataStore {
    std::string archive_dir_;
    std::string current_path_;
    FILE* current_file_ = nullptr;
    int64_t current_size_ = 0;
    int file_seq_ = 1;
    std::string current_month_;  // YYYYMM
    bool failed_ = false;
    std::unordered_set<std::string> validated_files_;

public:
    struct StoredRecord {
        std::string file_path;  // relative path within archive
        int64_t offset;         // byte offset within file
        uint32_t size;          // record size in bytes
        int file_seq = 1;       // data_NNNN.dat sequence within the month
        bool ok = false;        // true only after a complete write
    };

    explicit DataStore(const std::string& archive_dir);

    ~DataStore() { finish(); }

    // Compress and write an article. Returns the storage location.
    StoredRecord write_article(
        const std::string& url,
        uint32_t crawl_date,
        const std::string& title,
        const std::string& body);

    // Flush and close the current file. Call before publishing an index so a
    // delayed filesystem error cannot leave index entries pointing at lost data.
    bool finish();

private:
    static std::string date_to_month(uint32_t d);
    bool close_current();
    bool open_next();
    bool validate_existing_file(FILE* file, int64_t file_size,
                                const std::string& path) const;
    int latest_file_sequence() const;
};

#endif // STORE_H
