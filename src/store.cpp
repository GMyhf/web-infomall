/*
 * store.cpp — Write ArticleRecord to append-only data files.
 *
 * Organizes data files by year/month like Depot's DptGroupUp.
 * Each data file is a sequence of ArticleRecord binary blobs.
 */

#include "store.h"
#include <zlib.h>
#include <cstring>
#include <cstdio>
#include <dirent.h>

// ── DataFile Writer ───────────────────────────────────────────

DataStore::DataStore(const std::string& archive_dir)
    : archive_dir_(archive_dir) {
    // Create top-level directories once at construction
    if ((mkdir(archive_dir_.c_str(), 0755) != 0 && errno != EEXIST) ||
        (mkdir((archive_dir_ + "/data").c_str(), 0755) != 0 && errno != EEXIST)) {
        fprintf(stderr, "ERROR: Cannot create archive directory %s: %s\n",
                archive_dir_.c_str(), strerror(errno));
        failed_ = true;
    }
}

// Compress and write an article. Returns the storage location.
DataStore::StoredRecord DataStore::write_article(
    const std::string& url,
    uint32_t crawl_date,
    const std::string& title,
    const std::string& body)
{
    StoredRecord loc;
    if (failed_ || !valid_archive_url(url) || !valid_crawl_date(crawl_date)) return loc;

    if (url.size() > UINT32_MAX || title.size() > UINT32_MAX ||
        body.size() > MAX_BODY_SIZE) {
        fprintf(stderr, "ERROR: Article fields exceed the supported size limit\n");
        return loc;
    }

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
    uint64_t stored_size = static_cast<uint64_t>(ArticleRecord::HEADER_SIZE)
        + url.size() + title.size() + body_compr_len;
    if (stored_size > MAX_RECORD_SIZE) return loc;
    uint32_t record_size = static_cast<uint32_t>(stored_size);

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
    rec->mini_hash = mini_hash(url);
    rec->crc32 = 0;

    memcpy(const_cast<char*>(rec->url()), url.data(), url.size());
    memcpy(const_cast<char*>(rec->title()), title.data(), title.size());
    if (is_compressed) {
        memcpy(const_cast<char*>(rec->body()), compressed.data(), compressed.size());
    } else {
        memcpy(const_cast<char*>(rec->body()), body.data(), body.size());
    }

    // Compute CRC-32 over the full record
    rec->crc32 = compute_record_crc32(rec);

    // Open data file if needed
    std::string month = date_to_month(crawl_date);
    if (month != current_month_) {
        if (!close_current()) return loc;
        current_month_ = month;
        file_seq_ = latest_file_sequence();
        if (!open_next()) return loc;
    } else if (!current_file_) {
        if (!open_next()) return loc;
    }

    if (current_size_ + record_size > MAX_DAT_FILE) {
        if (!close_current()) return loc;
        file_seq_++;
        if (!open_next()) return loc;
    }

    // Write
    loc.file_path = current_path_;
    loc.offset = current_size_;
    loc.size = record_size;
    loc.file_seq = file_seq_;

    if (fwrite(buf.data(), 1, record_size, current_file_) != record_size) {
        fprintf(stderr, "ERROR: write failed for %s\n", current_path_.c_str());
        failed_ = true;
        return loc;
    }
    current_size_ += record_size;
    loc.ok = true;
    return loc;
}

std::string DataStore::date_to_month(uint32_t d) {
    // YYYYMMDD → YYYYMM
    char buf[16];
    snprintf(buf, sizeof(buf), "%04u%02u", d / 10000, (d / 100) % 100);
    return buf;
}

bool DataStore::close_current() {
    bool ok = true;
    if (current_file_) {
        int saved_errno = 0;
        if (fflush(current_file_) != 0) saved_errno = errno;
        if (saved_errno == 0 && !fsync_file_descriptor(fileno(current_file_)))
            saved_errno = errno;
        if (fclose(current_file_) != 0 && saved_errno == 0) saved_errno = errno;
        if (saved_errno != 0) {
            fprintf(stderr, "ERROR: Cannot flush %s: %s\n",
                    current_path_.c_str(), strerror(saved_errno));
            failed_ = true;
            ok = false;
        } else {
            const std::string month_dir = archive_dir_ + "/data/" + current_month_;
            if (!fsync_directory_path(month_dir)) {
                fprintf(stderr, "ERROR: Cannot sync %s: %s\n",
                        month_dir.c_str(), strerror(errno));
                failed_ = true;
                ok = false;
            }
        }
        current_file_ = nullptr;
    }
    current_size_ = 0;
    return ok;
}

bool DataStore::finish() {
    return close_current() && !failed_;
}

int DataStore::latest_file_sequence() const {
    std::string dir = archive_dir_ + "/data/" + current_month_;
    DIR* dp = opendir(dir.c_str());
    if (!dp) return 1;

    int latest = 1;
    while (dirent* ent = readdir(dp)) {
        int seq = 0;
        char trailing = 0;
        if (sscanf(ent->d_name, "data_%d.dat%c", &seq, &trailing) == 1 && seq > latest)
            latest = seq;
    }
    closedir(dp);
    return latest;
}

bool DataStore::validate_existing_file(FILE* file, int64_t file_size,
                                       const std::string& path) const {
    int64_t offset = 0;
    while (offset < file_size) {
        if (fseeko(file, offset, SEEK_SET) != 0) return false;
        ArticleRecord header = {};
        if (fread(&header, sizeof(header), 1, file) != 1 ||
            header.magic != ARTICLE_MAGIC ||
            header.record_size < ArticleRecord::HEADER_SIZE ||
            header.record_size > MAX_RECORD_SIZE ||
            !valid_crawl_date(header.crawl_date)) {
            fprintf(stderr, "ERROR: Invalid existing record at offset %lld in %s\n",
                    static_cast<long long>(offset), path.c_str());
            return false;
        }
        uint64_t payload_size = static_cast<uint64_t>(ArticleRecord::HEADER_SIZE)
            + header.url_len + header.title_len + header.body_compr_len;
        if (payload_size != header.record_size ||
            static_cast<uint64_t>(offset) + header.record_size >
                static_cast<uint64_t>(file_size)) {
            fprintf(stderr, "ERROR: Truncated existing record at offset %lld in %s\n",
                    static_cast<long long>(offset), path.c_str());
            return false;
        }
        offset += header.record_size;
    }
    return fseeko(file, 0, SEEK_END) == 0;
}

bool DataStore::open_next() {
    // Create monthly data directory
    std::string dir = archive_dir_ + "/data/" + current_month_;
    bool created_dir = mkdir(dir.c_str(), 0755) == 0;
    if (!created_dir && errno != EEXIST) {
        fprintf(stderr, "ERROR: Cannot create %s: %s\n", dir.c_str(), strerror(errno));
        failed_ = true;
        return false;
    }
    if (created_dir && !fsync_directory_path(archive_dir_ + "/data")) {
        fprintf(stderr, "ERROR: Cannot sync data directory: %s\n", strerror(errno));
        failed_ = true;
        return false;
    }

    char fname[64];
    snprintf(fname, sizeof(fname), "/data_%04d.dat", file_seq_);
    current_path_ = current_month_ + fname;
    std::string full = dir + fname;

    current_file_ = fopen(full.c_str(), "a+b");
    if (!current_file_) {
        fprintf(stderr, "ERROR: Cannot open %s: %s\n", full.c_str(), strerror(errno));
        failed_ = true;
        return false;
    }
    // Get current size
    if (fseeko(current_file_, 0, SEEK_END) != 0 ||
        (current_size_ = ftello(current_file_)) < 0) {
        fprintf(stderr, "ERROR: Cannot seek %s: %s\n", full.c_str(), strerror(errno));
        fclose(current_file_);
        current_file_ = nullptr;
        current_size_ = 0;
        failed_ = true;
        return false;
    }
    if (current_size_ > 0 && validated_files_.insert(full).second &&
        !validate_existing_file(current_file_, current_size_, full)) {
        fclose(current_file_);
        current_file_ = nullptr;
        current_size_ = 0;
        failed_ = true;
        return false;
    }
    return true;
}
