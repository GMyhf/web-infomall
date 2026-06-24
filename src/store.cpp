/*
 * store.cpp — Write ArticleRecord to append-only data files.
 *
 * Organizes data files by year/month like Depot's DptGroupUp.
 * Each data file is a sequence of ArticleRecord binary blobs.
 */

#include "common.h"
#include <zlib.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
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
    };

    explicit DataStore(const std::string& archive_dir)
        : archive_dir_(archive_dir) {}

    ~DataStore() { close_current(); }

    // Compress and write an article. Returns the storage location.
    StoredRecord write_article(
        const std::string& url,
        uint32_t crawl_date,
        const std::string& title,
        const std::string& body)
    {
        // Compress body with zlib
        std::vector<uint8_t> compressed;
        bool is_compressed = false;
        uint32_t body_orig_len = body.size();

        if (body.size() > 50) {  // Only compress if it's worth it
            uLongf bound = compressBound(body.size());
            compressed.resize(bound);
            int ret = ::compress(
                compressed.data(), &bound,
                reinterpret_cast<const Bytef*>(body.data()), body.size());
            if (ret == Z_OK && bound < body.size() * 0.95) {
                compressed.resize(bound);
                is_compressed = true;
            } else {
                compressed.clear();
            }
        }

        uint32_t body_compr_len = is_compressed ? static_cast<uint32_t>(compressed.size()) : body.size();
        uint32_t record_size = ArticleRecord::HEADER_SIZE
            + static_cast<uint32_t>(url.size())
            + static_cast<uint32_t>(title.size())
            + body_compr_len;

        // Allocate and fill record
        std::vector<char> buf(record_size, 0);
        auto* rec = reinterpret_cast<ArticleRecord*>(buf.data());
        rec->magic = ARTICLE_MAGIC;
        rec->flags = (is_compressed ? 1 : 0);
        rec->crawl_date = crawl_date;
        rec->url_len = url.size();
        rec->title_len = title.size();
        rec->body_compr_len = body_compr_len;
        rec->body_orig_len = body_orig_len;
        rec->record_size = record_size;
        rec->mini_md5 = mini_md5(url);
        rec->reserved = 0;

        memcpy(const_cast<char*>(rec->url()), url.data(), url.size());
        memcpy(const_cast<char*>(rec->title()), title.data(), title.size());
        if (is_compressed) {
            memcpy(const_cast<char*>(rec->body()), compressed.data(), compressed.size());
        } else {
            memcpy(const_cast<char*>(rec->body()), body.data(), body.size());
        }

        // Open data file if needed
        std::string month = date_to_month(crawl_date);
        if (month != current_month_ || !current_file_ || current_size_ + record_size > MAX_DAT_FILE) {
            if (month != current_month_) {
                close_current();
                current_month_ = month;
                file_seq_ = 1;
            } else if (current_size_ + record_size > MAX_DAT_FILE) {
                close_current();
                file_seq_++;
            }
            open_next();
        }

        // Write
        StoredRecord loc;
        loc.file_path = current_path_;
        loc.offset = current_size_;
        loc.size = record_size;

        if (fwrite(buf.data(), 1, record_size, current_file_) != record_size) {
            fprintf(stderr, "ERROR: write failed for %s\n", current_path_.c_str());
            return loc;
        }
        current_size_ += record_size;
        return loc;
    }

private:
    static std::string date_to_month(uint32_t d) {
        // YYYYMMDD → YYYYMM
        char buf[7];
        snprintf(buf, sizeof(buf), "%04u%02u", d / 10000, (d / 100) % 100);
        return buf;
    }

    void close_current() {
        if (current_file_) {
            fclose(current_file_);
            current_file_ = nullptr;
        }
        current_size_ = 0;
    }

    void open_next() {
        // Create directory: archive/data/YYYYMM/
        std::string dir = archive_dir_ + "/data/" + current_month_;
        mkdir(archive_dir_.c_str(), 0755);
        mkdir((archive_dir_ + "/data").c_str(), 0755);
        mkdir(dir.c_str(), 0755);

        char fname[64];
        snprintf(fname, sizeof(fname), "/data_%04d.dat", file_seq_);
        current_path_ = current_month_ + fname;
        std::string full = dir + fname;

        current_file_ = fopen(full.c_str(), "ab");
        if (!current_file_) {
            fprintf(stderr, "ERROR: Cannot open %s: %s\n", full.c_str(), strerror(errno));
            return;
        }
        // Get current size
        fseek(current_file_, 0, SEEK_END);
        current_size_ = ftell(current_file_);
    }
};
