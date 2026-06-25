/*
 * store.h — Append-only ArticleRecord data file writer.
 */

#ifndef STORE_H
#define STORE_H

#include "common.h"
#include <cstdio>
#include <string>
#include <vector>
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

public:
    struct StoredRecord {
        std::string file_path;  // relative path within archive
        int64_t offset;         // byte offset within file
        uint32_t size;          // record size in bytes
        bool ok = true;         // false if write failed
    };

    explicit DataStore(const std::string& archive_dir);

    ~DataStore() { close_current(); }

    // Compress and write an article. Returns the storage location.
    StoredRecord write_article(
        const std::string& url,
        uint32_t crawl_date,
        const std::string& title,
        const std::string& body);

private:
    static std::string date_to_month(uint32_t d);
    void close_current();
    void open_next();
};

#endif // STORE_H
