/*
 * query.cpp — QueryEngine: URL lookup, host search, prefix search.
 *
 * Uses mmap'd shard index files for zero-copy binary search.
 * Exact host/URL lookups use binary search. Prefix and substring discovery
 * perform bounded scans. v2 shards embed URLs directly in the index file
 * (url_pool), eliminating data-file IO for index-only searches.
 */

#include "query.h"
#include <cerrno>
#include <random>
#include <thread>

// ── MappedShard ────────────────────────────────────────────────

bool MappedShard::open(const char* path) {
    fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;

    auto fail = [this]() {
        if (data && data != MAP_FAILED) munmap(data, file_size);
        if (fd >= 0) ::close(fd);
        fd = -1;
        file_size = 0;
        data = nullptr;
        header = nullptr;
        hosts = nullptr;
        entries = nullptr;
        url_pool = nullptr;
        is_v2 = false;
        return false;
    };

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < static_cast<off_t>(sizeof(ShardFileHeader)))
        return fail();
    file_size = static_cast<size_t>(st.st_size);

    data = mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        data = nullptr;
        return fail();
    }

    header = static_cast<const ShardFileHeader*>(data);
    if (header->magic != SHARD_MAGIC && header->magic != SHARD_MAGIC_V1) {
        return fail();
    }
    is_v2 = (header->magic == SHARD_MAGIC);
    // Older builders could emit structurally valid empty shards.  Keep those
    // readable for compatibility, but reject inconsistent or padded empties;
    // QueryEngine::init() only counts shards that contain entries.
    if ((header->entry_count == 0) != (header->host_count == 0) ||
        (is_v2 && header->entry_count == 0 && header->url_pool_size != 0))
        return fail();

    uint64_t layout_size = sizeof(ShardFileHeader)
        + static_cast<uint64_t>(header->host_count) * sizeof(HostBlock)
        + static_cast<uint64_t>(header->entry_count) * sizeof(UrlIndexEntry);
    if (is_v2) layout_size += header->url_pool_size;
    if (layout_size > file_size || (is_v2 && layout_size != file_size)) return fail();

    hosts = reinterpret_cast<const HostBlock*>(header + 1);
    entries = reinterpret_cast<const UrlIndexEntry*>(hosts + header->host_count);

    // v2: URL pool follows entries array
    if (is_v2 && header->url_pool_size > 0) {
        url_pool = reinterpret_cast<const char*>(entries + header->entry_count);
    }

    if (is_v2) {
        for (uint32_t i = 0; i < header->entry_count; i++) {
            if (entries[i].url_len == 0 || entries[i].url_len > MAX_URL_LEN ||
                !entry_in_pool(entries[i], header->url_pool_size)) return fail();
        }
    }

    // Query paths rely on sorted, in-bounds host blocks and hash-sorted entries.
    // Validate these invariants once at startup rather than trusting mmap data on
    // every request.
    std::vector<std::pair<uint32_t, uint32_t>> intervals;
    intervals.reserve(header->host_count);
    for (uint32_t i = 0; i < header->host_count; i++) {
        const HostBlock& hb = hosts[i];
        if (hb.host[0] == '\0' || hb.entry_count == 0 ||
            static_cast<uint64_t>(hb.first_entry) + hb.entry_count > header->entry_count)
            return fail();
        intervals.emplace_back(hb.first_entry, hb.first_entry + hb.entry_count);
        if (i > 0 && strncmp(hosts[i - 1].host, hb.host, HOST_HASH_LEN) > 0)
            return fail();

        const UrlIndexEntry* block = entries + hb.first_entry;
        for (uint32_t j = 0; j < hb.entry_count; j++) {
            const UrlIndexEntry& ent = block[j];
            // Preserve legacy archive compatibility: some source snapshots
            // contain calendar-invalid YYYYMMDD values (for example 20030229).
            // The field is still bounded and sorted numerically; rejecting it
            // here would make otherwise structurally safe historical indexes
            // impossible to serve. Integrity verification reports these values
            // separately.
            if (j > 0) {
                const UrlIndexEntry& prev = block[j - 1];
                if (prev.url_hash > ent.url_hash ||
                    (prev.url_hash == ent.url_hash && prev.crawl_date < ent.crawl_date))
                    return fail();
            }
        }
    }
    std::sort(intervals.begin(), intervals.end());
    uint32_t covered = 0;
    for (const auto& interval : intervals) {
        if (interval.first != covered || interval.second < interval.first) return fail();
        covered = interval.second;
    }
    if (covered != header->entry_count) return fail();
    return true;
}

MappedShard::~MappedShard() {
    if (data && data != MAP_FAILED) munmap(data, file_size);
    if (fd >= 0) close(fd);
}

// ── ArticleReader ──────────────────────────────────────────────

ArticleReader::ArticleReader(const std::string& data_dir) : data_dir_(data_dir) {}
ArticleReader::~ArticleReader() { for (auto& kv : open_files_) ::close(kv.second); }

int ArticleReader::open_file(const std::string& rel_path) {
    std::lock_guard<std::mutex> lk(files_mtx_);
    auto it = open_files_.find(rel_path);
    if (it != open_files_.end()) return it->second;
    std::string full = data_dir_ + "/" + rel_path;
    int fd = ::open(full.c_str(), O_RDONLY);
    if (fd >= 0) open_files_[rel_path] = fd;
    return fd;
}

ArticleReader::Article ArticleReader::read_article(
    const std::string& rel_path, int64_t offset, uint32_t size) {
    Article art = {};
    art.valid = false;
    if (offset < 0) return art;

    uint32_t initial_size = std::max<uint32_t>(size, ArticleRecord::HEADER_SIZE);
    if (initial_size > MAX_RECORD_SIZE) return art;

    std::vector<char> buf(initial_size);
    int fd = open_file(rel_path);
    if (fd < 0) return art;
    // pread carries its own offset, so concurrent workers can share the fd.
    ssize_t n = pread(fd, buf.data(), initial_size, offset);
    if (n != static_cast<ssize_t>(initial_size)) return art;

    auto* rec = reinterpret_cast<const ArticleRecord*>(buf.data());
    if (rec->magic != ARTICLE_MAGIC) return art;

    // The index stores record_size as uint16, so records over 64 KiB arrive
    // modulo 65536 (occasionally even below the header size). The on-disk header
    // is authoritative once its bounds have been checked.
    if (rec->record_size < ArticleRecord::HEADER_SIZE || rec->record_size > MAX_RECORD_SIZE)
        return art;
    if (rec->record_size > initial_size) {
        size = rec->record_size;
        buf.resize(size);
        if (pread(fd, buf.data(), size, offset) != static_cast<ssize_t>(size)) return art;
        rec = reinterpret_cast<const ArticleRecord*>(buf.data());
    } else {
        size = rec->record_size;
        buf.resize(size);
    }
    // Bounds-check the variable-length sections against the buffer we hold,
    // so corrupt length fields can't cause out-of-bounds reads below.
    uint64_t need = static_cast<uint64_t>(ArticleRecord::HEADER_SIZE)
                  + rec->url_len + rec->title_len + rec->body_compr_len;
    if (rec->record_size > size || need != rec->record_size) {
        return art;
    }

    bool integrity_ok = true;

    // Verify CRC-32 if present (non-zero means it was computed)
    if (rec->crc32 != 0) {
        uint32_t expected = compute_record_crc32(rec);
        if (expected != rec->crc32) {
            fprintf(stderr, "CRC32 MISMATCH at offset %lld in %s: got 0x%08x, expected 0x%08x\n",
                    (long long)offset, rel_path.c_str(), rec->crc32, expected);
            // Surface corruption to the caller. Data is still populated below
            // so integrity tooling can inspect it, but the page is not served.
            integrity_ok = false;
        }
    }

    art.url.assign(rec->url(), rec->url_len);
    art.title.assign(rec->title(), rec->title_len);
    art.date = rec->crawl_date;

    bool compressed = (rec->flags & 1);
    if (compressed && rec->body_compr_len > 0) {
        // Cap the decompression buffer so a corrupt body_orig_len can't
        // trigger a multi-GB allocation.
        if (rec->body_orig_len > MAX_BODY_SIZE) {
            return art;
        }
        std::vector<char> decomp(rec->body_orig_len + 1);
        uLongf dest_len = rec->body_orig_len;
        int ret = ::uncompress(
            reinterpret_cast<Bytef*>(decomp.data()), &dest_len,
            reinterpret_cast<const Bytef*>(rec->body()), rec->body_compr_len);
        if (ret == Z_OK) {
            decomp.resize(dest_len);
            art.body.assign(decomp.data(), decomp.size());
        } else {
            fprintf(stderr, "DECOMPRESS FAILED (zlib %d) at offset %lld in %s\n",
                    ret, (long long)offset, rel_path.c_str());
            // Body is unrecoverable; expose compressed bytes for diagnostics
            // but mark invalid so the server does not render garbage.
            art.body.assign(rec->body(), rec->body_compr_len);
            integrity_ok = false;
        }
    } else {
        art.body.assign(rec->body(), rec->body_compr_len);
    }
    art.valid = integrity_ok;
    return art;
}

// ── Query Engine ──────────────────────────────────────────────

std::string QueryEngine::data_path(uint32_t crawl_date, uint32_t file_seq) {
    // Legacy index entries carry reserved == 0; those predate multi-file
    // months and always live in data_0001.dat.
    if (file_seq == 0) file_seq = 1;
    char buf[128];
    snprintf(buf, sizeof(buf), "%04u%02u/data_%04u.dat",
             crawl_date / 10000, (crawl_date / 100) % 100, file_seq);
    return buf;
}

QueryEngine::QueryEngine(const std::string& data_dir, const std::string& index_dir)
    : data_dir_(data_dir), index_dir_(index_dir), reader_(data_dir) {}

bool QueryEngine::init() {
    for (int i = 0; i < NUM_SHARDS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/url_%02d.idx", index_dir_.c_str(), i);
        struct stat st;
        if (stat(path, &st) != 0) {
            if (errno != ENOENT) {
                fprintf(stderr, "ERROR: Cannot stat shard %s: %s\n", path, strerror(errno));
                return false;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode) || !shards_[i].open(path)) {
            fprintf(stderr, "ERROR: Invalid shard index %s\n", path);
            return false;
        }
        if (shards_[i].header->entry_count > 0) shards_loaded_++;
    }
    printf("Loaded %d/%d shard index files\n", shards_loaded_, NUM_SHARDS);
    if (shards_loaded_ == 0) return false;

    // Load precomputed auxiliary data
    load_year_dist();
    load_today();
    load_title_index();

    // Populate immutable stats before server worker threads begin reading them.
    uint32_t total, hosts, date_min, date_max;
    get_stats(total, hosts, date_min, date_max);

    return true;
}

// ── Precomputed data loading ──────────────────────────────────

static uint64_t open_file_size(FILE* f) {
    struct stat st;
    return fstat(fileno(f), &st) == 0 && st.st_size >= 0
        ? static_cast<uint64_t>(st.st_size) : 0;
}

void QueryEngine::load_year_dist() {
    char path[256];
    snprintf(path, sizeof(path), "%s/year_dist.dat", index_dir_.c_str());
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint64_t file_size = open_file_size(f);
    uint32_t count;
    if (fread(&count, sizeof(count), 1, f) != 1) { fclose(f); return; }
    if (count > 10000 || sizeof(count) + static_cast<uint64_t>(count) * 8 != file_size) {
        fprintf(stderr, "WARNING: Ignoring invalid year_dist.dat\n");
        fclose(f);
        return;
    }
    std::vector<YearCount> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t year, cnt;
        if (fread(&year, sizeof(year), 1, f) != 1 ||
            fread(&cnt, sizeof(cnt), 1, f) != 1 || year == 0 || year > 9999) {
            loaded.clear();
            break;
        }
        loaded.push_back({year, cnt});
    }
    fclose(f);
    if (loaded.size() != count ||
        !std::is_sorted(loaded.begin(), loaded.end(),
            [](const YearCount& a, const YearCount& b) { return a.year < b.year; }) ||
        std::adjacent_find(loaded.begin(), loaded.end(),
            [](const YearCount& a, const YearCount& b) { return a.year == b.year; }) != loaded.end()) {
        fprintf(stderr, "WARNING: Ignoring truncated or unsorted year_dist.dat\n");
        return;
    }
    year_dist_cached_ = std::move(loaded);
    printf("  Loaded %zu year distribution entries\n", year_dist_cached_.size());
}

void QueryEngine::load_today() {
    char path[256];
    snprintf(path, sizeof(path), "%s/today.dat", index_dir_.c_str());
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint32_t num_days;
    if (fread(&num_days, sizeof(num_days), 1, f) != 1) { fclose(f); return; }
    if (num_days > 366) {
        fprintf(stderr, "WARNING: Ignoring invalid today.dat\n");
        fclose(f);
        return;
    }
    std::vector<TodayEntry> loaded;
    loaded.reserve(num_days);
    bool ok = true;
    uint16_t previous_mmdd = 0;
    for (uint32_t i = 0; i < num_days; i++) {
        TodayEntry te;
        if (fread(&te.mmdd, sizeof(te.mmdd), 1, f) != 1 ||
            (!valid_crawl_date(20000000u + te.mmdd) &&
             !valid_crawl_date(20010000u + te.mmdd)) ||
            (i > 0 && te.mmdd <= previous_mmdd)) { ok = false; break; }
        previous_mmdd = te.mmdd;
        uint32_t url_count;
        if (fread(&url_count, sizeof(url_count), 1, f) != 1 || url_count > 200) {
            ok = false;
            break;
        }
        te.urls.reserve(url_count);
        for (uint32_t j = 0; j < url_count; j++) {
            uint16_t len;
            if (fread(&len, sizeof(len), 1, f) != 1 || len == 0 || len > MAX_URL_LEN) {
                ok = false;
                break;
            }
            std::string url(len, '\0');
            if (fread(url.data(), 1, len, f) != len || !valid_archive_url(url)) {
                ok = false;
                break;
            }
            te.urls.push_back(std::move(url));
        }
        if (!ok) break;
        loaded.push_back(std::move(te));
    }
    if (ok) {
        off_t end = ftello(f);
        ok = end >= 0 && static_cast<uint64_t>(end) == open_file_size(f);
    }
    fclose(f);
    if (!ok || loaded.size() != num_days) {
        fprintf(stderr, "WARNING: Ignoring truncated or invalid today.dat\n");
        return;
    }
    today_data_ = std::move(loaded);
    printf("  Loaded %zu today-in-history entries\n", today_data_.size());
}

// ── URL resolve helpers ──────────────────────────────────────

std::string QueryEngine::get_entry_url(const UrlIndexEntry& ent, int sid) {
    auto& shard = shards_[sid];
    if (shard.is_v2 && shard.url_pool) {
        return entry_url(ent, shard.url_pool, shard.header->url_pool_size);
    }
    // v1 fallback: read from data file
    auto buf = reader_.read_article(data_path(ent.crawl_date, ent.reserved),
                                     ent.file_offset, ent.record_size);
    return buf.url;
}

static std::pair<uint32_t, uint32_t> find_host_range(
        const HostBlock* hosts, uint32_t count, const std::string& host) {
    char key[HOST_HASH_LEN] = {};
    strncpy(key, host.c_str(), HOST_HASH_LEN - 1);

    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (strncmp(hosts[mid].host, key, HOST_HASH_LEN) < 0) lo = mid + 1;
        else hi = mid;
    }
    uint32_t end = lo;
    while (end < count && strncmp(hosts[end].host, key, HOST_HASH_LEN) == 0) end++;
    return {lo, end};
}

static std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// ── Single-page lookup ───────────────────────────────────────

ArticleReader::Article QueryEngine::get_page(const std::string& url, uint32_t date) {
    std::string host = extract_host(url);
    if (!valid_archive_url(url)) return {};
    int sid = shard_for_host(host);
    if (sid < 0 || sid >= NUM_SHARDS) return {};

    auto& shard = shards_[sid];
    if (!shard.data) return {};

    uint64_t hash = ::url_hash(url);
    auto range = find_host_range(shard.hosts, shard.header->host_count, host);
    const UrlIndexEntry* best = nullptr;
    uint64_t best_diff = UINT64_MAX;
    int64_t requested_day = crawl_date_ordinal(date);

    for (uint32_t h = range.first; h < range.second; h++) {
        const HostBlock& hb = shard.hosts[h];
        const UrlIndexEntry* block = shard.entries + hb.first_entry;
        const UrlIndexEntry* first = find_first(block, hb.entry_count, hash);
        if (!first) continue;

        uint32_t idx = static_cast<uint32_t>(first - block);
        while (idx < hb.entry_count && block[idx].url_hash == hash) {
            const UrlIndexEntry& ent = block[idx++];
            if (get_entry_url(ent, sid) != url) continue;
            if (date == 0) {
                if (!best || ent.crawl_date > best->crawl_date) best = &ent;
                continue;
            }

            int64_t entry_day = crawl_date_ordinal(ent.crawl_date);
            uint64_t diff = (requested_day >= 0 && entry_day >= 0)
                ? static_cast<uint64_t>(entry_day > requested_day
                    ? entry_day - requested_day : requested_day - entry_day)
                : static_cast<uint64_t>(ent.crawl_date > date
                    ? ent.crawl_date - date : date - ent.crawl_date);
            if (!best || diff < best_diff ||
                (diff == best_diff && ent.crawl_date > best->crawl_date)) {
                best = &ent;
                best_diff = diff;
            }
        }
    }
    if (!best) return {};
    return reader_.read_article(data_path(best->crawl_date, best->reserved),
                                best->file_offset, best->record_size);
}

// ── Version listing ──────────────────────────────────────────

std::vector<QueryEngine::Version> QueryEngine::get_versions(const std::string& url) {
    std::vector<Version> vers;
    std::string host = extract_host(url);
    if (!valid_archive_url(url)) return vers;
    int sid = shard_for_host(host);
    if (sid < 0 || sid >= NUM_SHARDS) return vers;

    auto& shard = shards_[sid];
    if (!shard.data) return vers;

    uint64_t hash = ::url_hash(url);
    auto range = find_host_range(shard.hosts, shard.header->host_count, host);
    std::map<uint32_t, int, std::greater<uint32_t>> counts;
    for (uint32_t h = range.first; h < range.second; h++) {
        const HostBlock& hb = shard.hosts[h];
        const UrlIndexEntry* block = shard.entries + hb.first_entry;
        const UrlIndexEntry* first = find_first(block, hb.entry_count, hash);
        if (!first) continue;
        uint32_t idx = static_cast<uint32_t>(first - block);
        while (idx < hb.entry_count && block[idx].url_hash == hash) {
            const UrlIndexEntry& ent = block[idx++];
            if (get_entry_url(ent, sid) == url) counts[ent.crawl_date]++;
        }
    }
    for (const auto& item : counts) vers.push_back({item.first, item.second});
    return vers;
}

// ── Host search (substring) ──────────────────────────────────

std::vector<std::pair<std::string, uint32_t>> QueryEngine::search_host_substring(
        const std::string& substr, int limit) {
    std::vector<std::pair<std::string, uint32_t>> results;
    if (limit <= 0) return results;
    std::string needle = lowercase_ascii(substr);
    std::unordered_set<std::string> seen;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;
        for (uint32_t i = 0; i < shard.header->host_count &&
             results.size() < static_cast<size_t>(limit); i++) {
            const HostBlock& hb = shard.hosts[i];
            std::string host_name;
            if (hb.entry_count > 0)
                host_name = extract_host(get_entry_url(shard.entries[hb.first_entry], sid));
            if (host_name.empty())
                host_name.assign(hb.host, strnlen(hb.host, HOST_HASH_LEN));
            if (host_name.find(needle) != std::string::npos && seen.insert(host_name).second) {
                results.emplace_back(host_name, shard.hosts[i].entry_count);
            }
        }
    }
    return results;
}

// ── Host URL listing ─────────────────────────────────────────

std::vector<QueryEngine::UrlWithDate> QueryEngine::get_host_urls(
        const std::string& host, int limit) {
    std::vector<UrlWithDate> urls;
    if (limit <= 0) return urls;
    std::string normalized_host = lowercase_ascii(host);
    int sid = shard_for_host(normalized_host);
    if (sid < 0 || sid >= NUM_SHARDS) return urls;

    auto& shard = shards_[sid];
    if (!shard.data) return urls;

    auto range = find_host_range(shard.hosts, shard.header->host_count, normalized_host);
    std::unordered_set<std::string> seen;
    for (uint32_t h = range.first; h < range.second &&
         urls.size() < static_cast<size_t>(limit); h++) {
        const HostBlock& hb = shard.hosts[h];
        const UrlIndexEntry* block = shard.entries + hb.first_entry;
        for (uint32_t i = 0; i < hb.entry_count &&
             urls.size() < static_cast<size_t>(limit); i++) {
            const UrlIndexEntry& ent = block[i];
            std::string url = get_entry_url(ent, sid);
            if (!url.empty() && extract_host(url) == normalized_host && seen.insert(url).second)
                urls.push_back({std::move(url), ent.crawl_date});
        }
    }
    return urls;
}

QueryEngine::HostSummary QueryEngine::get_host_summary(const std::string& host) {
    HostSummary summary;
    std::string normalized_host = lowercase_ascii(host);
    int sid = shard_for_host(normalized_host);
    if (sid < 0 || sid >= NUM_SHARDS) return summary;

    auto& shard = shards_[sid];
    if (!shard.data) return summary;
    auto range = find_host_range(shard.hosts, shard.header->host_count, normalized_host);
    uint64_t current_hash = 0;
    bool have_hash = false;
    std::unordered_set<std::string> urls_for_hash;

    for (uint32_t h = range.first; h < range.second; h++) {
        const HostBlock& hb = shard.hosts[h];
        const UrlIndexEntry* block = shard.entries + hb.first_entry;
        for (uint32_t i = 0; i < hb.entry_count; i++) {
            const UrlIndexEntry& ent = block[i];
            std::string url = get_entry_url(ent, sid);
            if (url.empty() || extract_host(url) != normalized_host) continue;
            summary.record_count++;
            if (!have_hash || ent.url_hash != current_hash) {
                urls_for_hash.clear();
                current_hash = ent.url_hash;
                have_hash = true;
            }
            if (urls_for_hash.insert(url).second) {
                summary.unique_url_count++;
            }
            if (valid_crawl_date(ent.crawl_date)) {
                if (summary.date_min == 0 || ent.crawl_date < summary.date_min)
                    summary.date_min = ent.crawl_date;
                if (ent.crawl_date > summary.date_max) summary.date_max = ent.crawl_date;
                summary.year_counts[ent.crawl_date / 10000]++;
            }
        }
    }
    return summary;
}

// ── URL prefix search ────────────────────────────────────────

std::vector<std::string> QueryEngine::search_prefix(const std::string& prefix, int limit) {
    std::vector<std::string> urls;
    if (prefix.empty() || limit <= 0) return urls;

    for (int sid = 0; sid < NUM_SHARDS && urls.size() < static_cast<size_t>(limit); sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;

        if (shard.is_v2 && shard.url_pool) {
            for (uint32_t i = 0; i < shard.header->entry_count &&
                 urls.size() < static_cast<size_t>(limit); i++) {
                auto& ent = shard.entries[i];
                if (entry_url_has_prefix(ent, shard.url_pool, prefix, shard.header->url_pool_size) ||
                    entry_host_contains(ent, shard.url_pool, prefix, shard.header->url_pool_size)) {
                    std::string u = entry_url(ent, shard.url_pool, shard.header->url_pool_size);
                    if (!u.empty() && (urls.empty() || u != urls.back())) {
                        urls.push_back(u);
                    }
                }
            }
        } else {
            // v1 fallback: need data-file reads
            for (uint32_t i = 0; i < shard.header->entry_count &&
                 urls.size() < static_cast<size_t>(limit); i++) {
                auto& ent = shard.entries[i];
                auto art = reader_.read_article(data_path(ent.crawl_date, ent.reserved),
                    ent.file_offset,
                    std::min<uint32_t>(ent.record_size, ArticleRecord::HEADER_SIZE + 1024u));
                if (!art.url.empty() &&
                    (art.url.find(prefix) == 0 || extract_host(art.url).find(prefix) == 0)) {
                    if (urls.empty() || art.url != urls.back()) urls.push_back(art.url);
                }
            }
        }
    }
    return urls;
}

// ── Stats ────────────────────────────────────────────────────

void QueryEngine::get_stats(uint32_t& total_articles, uint32_t& total_urls,
                            uint32_t& date_min, uint32_t& date_max) {
    if (stats_cached_) {
        total_articles = total_articles_cached_;
        total_urls = total_hosts_cached_;
        date_min = date_min_cached_;
        date_max = date_max_cached_;
        return;
    }

    // Fast path: read from meta.dat
    char meta_path[256];
    snprintf(meta_path, sizeof(meta_path), "%s/meta.dat", index_dir_.c_str());
    FILE* mf = fopen(meta_path, "rb");
    if (mf) {
        ArchiveMeta meta;
        if (fread(&meta, sizeof(meta), 1, mf) == 1) {
            total_articles = meta.total_articles;
            total_urls = meta.total_urls;
            date_min = meta.date_min;
            date_max = meta.date_max;
            fclose(mf);
            if (total_urls > 0 && valid_crawl_date(date_min) && valid_crawl_date(date_max)) {
                total_articles_cached_ = total_articles;
                total_hosts_cached_ = total_urls;
                date_min_cached_ = date_min;
                date_max_cached_ = date_max;
                stats_cached_ = true;
                return;
            }
        } else {
            fclose(mf);
        }
    }
    // Slow path: compute from shard headers
    total_articles = 0; total_urls = 0;
    date_min = UINT32_MAX; date_max = 0;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;
        total_articles += shard.header->entry_count;
        total_urls += shard.header->host_count;
        for (uint32_t i = 0; i < shard.header->entry_count; i++) {
            uint32_t d = shard.entries[i].crawl_date;
            if (!valid_crawl_date(d)) continue;
            if (d < date_min) date_min = d;
            if (d > date_max) date_max = d;
        }
    }
    if (date_min == UINT32_MAX) date_min = 0;
    total_articles_cached_ = total_articles;
    total_hosts_cached_ = total_urls;
    date_min_cached_ = date_min;
    date_max_cached_ = date_max;
    stats_cached_ = true;
}

// ── Top hosts ────────────────────────────────────────────────

std::vector<std::pair<std::string, uint32_t>> QueryEngine::get_top_hosts(int limit) {
    std::vector<std::pair<std::string, uint32_t>> hosts;
    if (limit <= 0) return hosts;
    std::unordered_map<std::string, uint32_t> counts;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;
        for (uint32_t i = 0; i < shard.header->host_count; i++) {
            const HostBlock& hb = shard.hosts[i];
            std::string name;
            if (hb.entry_count > 0)
                name = extract_host(get_entry_url(shard.entries[hb.first_entry], sid));
            if (name.empty()) name.assign(hb.host, strnlen(hb.host, HOST_HASH_LEN));
            counts[name] += hb.entry_count;
        }
    }
    hosts.reserve(counts.size());
    for (auto& item : counts) hosts.push_back(item);
    auto by_count_desc = [](const auto& a, const auto& b) { return a.second > b.second; };
    if (hosts.size() > static_cast<size_t>(limit)) {
        // Only the top `limit` need ordering — O(n log k) instead of O(n log n).
        std::partial_sort(hosts.begin(), hosts.begin() + limit, hosts.end(), by_count_desc);
        hosts.resize(limit);
    } else {
        std::sort(hosts.begin(), hosts.end(), by_count_desc);
    }
    return hosts;
}

// ── Random URL ───────────────────────────────────────────────

std::string QueryEngine::get_random_url() {
    uint64_t total_entries = 0;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        if (shards_[sid].data) total_entries += shards_[sid].header->entry_count;
    }
    if (total_entries == 0) return "";

    static thread_local std::mt19937 generator(
        static_cast<uint32_t>(time(nullptr)) ^ static_cast<uint32_t>(getpid()) ^
        static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
    std::uniform_int_distribution<uint64_t> entry_pick(0, total_entries - 1);
    uint64_t selected = entry_pick(generator);
    int sid = 0;
    for (; sid < NUM_SHARDS; sid++) {
        if (!shards_[sid].data) continue;
        uint32_t count = shards_[sid].header->entry_count;
        if (selected < count) break;
        selected -= count;
    }
    if (sid == NUM_SHARDS) return "";
    auto& shard = shards_[sid];
    uint32_t idx = static_cast<uint32_t>(selected);
    auto& ent = shard.entries[idx];

    if (shard.is_v2 && shard.url_pool)
        return entry_url(ent, shard.url_pool, shard.header->url_pool_size);
    // v1 fallback
    return reader_.read_article(data_path(ent.crawl_date, ent.reserved),
        ent.file_offset, std::min<uint32_t>(ent.record_size,
            ArticleRecord::HEADER_SIZE + 1024u)).url;
}

// ── Year distribution (precomputed, fallback to scan) ────────

std::vector<QueryEngine::YearCount> QueryEngine::get_year_distribution() {
    std::lock_guard<std::mutex> lk(year_dist_mtx_);
    if (year_dist_cached_.empty())
        get_year_distribution_slow();   // fills year_dist_cached_ under lock
    return year_dist_cached_;
}

// Caller must hold year_dist_mtx_. Fills year_dist_cached_ by scanning shards.
std::vector<QueryEngine::YearCount> QueryEngine::get_year_distribution_slow() {
    std::map<uint32_t, uint32_t> ymap;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;
        for (uint32_t i = 0; i < shard.header->entry_count; i++) {
            if (!valid_crawl_date(shard.entries[i].crawl_date)) continue;
            uint32_t y = shard.entries[i].crawl_date / 10000;
            ymap[y]++;
        }
    }
    for (auto& kv : ymap)
        year_dist_cached_.push_back({kv.first, kv.second});
    return year_dist_cached_;
}

// ── Today in history (precomputed, fallback to scan) ─────────

std::vector<std::string> QueryEngine::get_today_in_history(uint32_t mmdd, int limit) {
    if (limit <= 0) return {};
    // Fast path: use precomputed data
    if (!today_data_.empty()) {
        for (auto& te : today_data_) {
            if (te.mmdd == mmdd) {
                std::vector<std::string> result;
                for (auto& url : te.urls) {
                    result.push_back(url);
                    if (result.size() >= static_cast<size_t>(limit)) break;
                }
                return result;
            }
        }
        return {}; // MMDD not in precomputed data
    }
    return get_today_in_history_slow(mmdd, limit);
}

std::vector<std::string> QueryEngine::get_today_in_history_slow(uint32_t mmdd, int limit) {
    std::vector<std::string> urls;
    if (limit <= 0 ||
        (!valid_crawl_date(20000000u + mmdd) &&
         !valid_crawl_date(20010000u + mmdd))) return urls;

    struct Candidate {
        std::string url;
        uint32_t date;
    };
    std::vector<Candidate> candidates;

    for (int sid = 0; sid < NUM_SHARDS && candidates.size() < 5000; sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;
        for (uint32_t i = 0; i < shard.header->entry_count &&
             candidates.size() < 5000; i++) {
            auto& ent = shard.entries[i];
            if ((ent.crawl_date % 10000) == mmdd) {
                std::string url;
                if (shard.is_v2 && shard.url_pool)
                    url = entry_url(ent, shard.url_pool, shard.header->url_pool_size);
                else {
                    url = reader_.read_article(data_path(ent.crawl_date, ent.reserved),
                        ent.file_offset, std::min<uint32_t>(ent.record_size,
                            ArticleRecord::HEADER_SIZE + 1024u)).url;
                }
                if (!url.empty())
                    candidates.push_back({url, ent.crawl_date});
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.date > b.date; });

    std::unordered_set<std::string> seen;
    for (auto& c : candidates) {
        if (!seen.insert(c.url).second) continue;
        urls.push_back(c.url);
        if (urls.size() >= static_cast<size_t>(limit)) break;
    }
    return urls;
}

// ── Browse by date ───────────────────────────────────────────

std::vector<QueryEngine::UrlWithDate> QueryEngine::get_by_date(uint32_t date, int limit) {
    std::vector<UrlWithDate> results;
    if (limit <= 0 || !valid_crawl_date(date)) return results;
    std::unordered_set<std::string> seen;
    for (int sid = 0; sid < NUM_SHARDS && results.size() < static_cast<size_t>(limit); sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;
        for (uint32_t i = 0; i < shard.header->entry_count &&
             results.size() < static_cast<size_t>(limit); i++) {
            auto& ent = shard.entries[i];
            if (ent.crawl_date == date) {
                std::string url;
                if (shard.is_v2 && shard.url_pool)
                    url = entry_url(ent, shard.url_pool, shard.header->url_pool_size);
                else {
                    url = reader_.read_article(data_path(ent.crawl_date, ent.reserved),
                        ent.file_offset, std::min<uint32_t>(ent.record_size,
                            ArticleRecord::HEADER_SIZE + 1024u)).url;
                }
                if (!url.empty() && seen.insert(url).second)
                    results.push_back({url, ent.crawl_date});
            }
        }
    }
    return results;
}

// ── Get article by exact URL + date ──────────────────────────

ArticleReader::Article QueryEngine::get_page_by_date(const std::string& url, uint32_t date) {
    std::string host = extract_host(url);
    if (!valid_archive_url(url) || !valid_crawl_date(date)) return {};
    int sid = shard_for_host(host);
    if (sid < 0 || sid >= NUM_SHARDS) return {};

    auto& shard = shards_[sid];
    if (!shard.data) return {};

    uint64_t hash = ::url_hash(url);
    auto range = find_host_range(shard.hosts, shard.header->host_count, host);
    for (uint32_t h = range.first; h < range.second; h++) {
        const HostBlock& hb = shard.hosts[h];
        const UrlIndexEntry* block = shard.entries + hb.first_entry;
        const UrlIndexEntry* first = find_first(block, hb.entry_count, hash);
        if (!first) continue;
        uint32_t idx = static_cast<uint32_t>(first - block);
        while (idx < hb.entry_count && block[idx].url_hash == hash) {
            const UrlIndexEntry& ent = block[idx++];
            if (ent.crawl_date == date && get_entry_url(ent, sid) == url) {
                return reader_.read_article(data_path(ent.crawl_date, ent.reserved),
                                            ent.file_offset, ent.record_size);
            }
        }
    }
    return {};
}

// ── Title index loading ─────────────────────────────────────

void QueryEngine::load_title_index() {
    char path[256];
    snprintf(path, sizeof(path), "%s/title_idx.dat", index_dir_.c_str());
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("  No title index found (title_idx.dat missing)\n");
        printf("  Run ./load to rebuild the archive with title indexing.\n");
        return;
    }

    uint32_t num_terms;
    if (fread(&num_terms, sizeof(num_terms), 1, f) != 1) { fclose(f); return; }
    uint64_t file_size = open_file_size(f);
    if (num_terms > (file_size >= 4 ? (file_size - 4) / 6 : 0)) {
        fprintf(stderr, "WARNING: Ignoring invalid title_idx.dat\n");
        fclose(f);
        return;
    }
    std::vector<TitleTerm> loaded;
    loaded.reserve(num_terms);
    bool ok = true;

    for (uint32_t i = 0; i < num_terms; i++) {
        TitleTerm tt;
        uint16_t tlen;
        if (fread(&tlen, sizeof(tlen), 1, f) != 1 || tlen == 0 || tlen > 4096) {
            ok = false;
            break;
        }
        tt.term.resize(tlen);
        if (fread(tt.term.data(), 1, tlen, f) != tlen) { ok = false; break; }

        uint32_t num_posts;
        if (fread(&num_posts, sizeof(num_posts), 1, f) != 1 ||
            num_posts > static_cast<uint32_t>(TITLE_INDEX_TOP_K)) {
            ok = false;
            break;
        }
        tt.postings.reserve(num_posts);

        for (uint32_t j = 0; j < num_posts; j++) {
            TitlePosting tp;
            uint16_t ulen, tlen2;
            if (fread(&tp.date, sizeof(tp.date), 1, f) != 1 ||
                !valid_crawl_date(tp.date) ||
                fread(&ulen, sizeof(ulen), 1, f) != 1 ||
                ulen == 0 || ulen > MAX_URL_LEN) { ok = false; break; }
            tp.url.resize(ulen);
            if (fread(tp.url.data(), 1, ulen, f) != ulen || !valid_archive_url(tp.url) ||
                fread(&tlen2, sizeof(tlen2), 1, f) != 1) { ok = false; break; }
            tp.title.resize(tlen2);
            if (tlen2 && fread(tp.title.data(), 1, tlen2, f) != tlen2) {
                ok = false;
                break;
            }
            tt.postings.push_back(std::move(tp));
        }
        if (!ok || tt.postings.size() != num_posts) { ok = false; break; }
        loaded.push_back(std::move(tt));
    }
    if (ok) {
        off_t end = ftello(f);
        ok = end >= 0 && static_cast<uint64_t>(end) == file_size;
    }
    fclose(f);
    if (!ok || loaded.size() != num_terms ||
        !std::is_sorted(loaded.begin(), loaded.end(),
            [](const TitleTerm& a, const TitleTerm& b) { return a.term < b.term; })) {
        fprintf(stderr, "WARNING: Ignoring truncated or unsorted title_idx.dat\n");
        return;
    }
    title_index_ = std::move(loaded);
    printf("  Loaded title index: %zu terms, %.0f KB\n",
           title_index_.size(),
           title_index_.size() * 24.0 / 1024.0);
}

const std::vector<TitlePosting>* QueryEngine::find_term(const std::string& term) const {
    int lo = 0, hi = static_cast<int>(title_index_.size());
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int cmp = title_index_[mid].term.compare(term);
        if (cmp < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo < static_cast<int>(title_index_.size()) && title_index_[lo].term == term)
        return &title_index_[lo].postings;
    return nullptr;
}

// ── Search by title token (hot topic) ────────────────────────

std::vector<TitlePosting> QueryEngine::search_by_title(const std::string& query, int limit) {
    std::vector<TitlePosting> results;
    if (title_index_.empty() || limit <= 0) return results;

    auto terms = tokenize_title(query);
    if (terms.empty()) return results;

    // Multi-term: AND logic — URL must match ALL terms
    // Start with the smallest posting set (most selective)
    std::vector<const std::vector<TitlePosting>*> postings_lists;
    for (auto& term : terms) {
        auto* posts = find_term(term);
        if (!posts) return results; // any term missing -> no results
        postings_lists.push_back(posts);
    }

    // Sort by list size ascending for efficiency
    std::sort(postings_lists.begin(), postings_lists.end(),
        [](const auto* a, const auto* b) { return a->size() < b->size(); });

    // Build a URL hash set for each of the larger lists once (O(total)), then
    // probe the smallest list against them in O(1) per term — turns the old
    // O(n·m·k) nested scan into O(n·m).
    std::vector<std::unordered_set<std::string>> url_sets;
    url_sets.reserve(postings_lists.size() - 1);
    for (size_t i = 1; i < postings_lists.size(); i++) {
        std::unordered_set<std::string> s;
        s.reserve(postings_lists[i]->size());
        for (auto& q : *postings_lists[i]) s.insert(q.url);
        url_sets.push_back(std::move(s));
    }

    auto& smallest = *postings_lists[0];
    std::map<std::string, std::pair<uint32_t, std::string>> candidates;

    for (auto& p : smallest) {
        bool found_in_all = true;
        for (auto& s : url_sets) {
            if (s.find(p.url) == s.end()) { found_in_all = false; break; }
        }
        if (found_in_all) {
            auto it = candidates.find(p.url);
            if (it == candidates.end() || p.date > it->second.first)
                candidates[p.url] = {p.date, p.title};
        }
    }

    for (auto& [url, info] : candidates)
        results.push_back({url, info.first, info.second});

    std::sort(results.begin(), results.end(),
        [](const TitlePosting& a, const TitlePosting& b) {
            return a.date > b.date;
        });
    if (results.size() > static_cast<size_t>(limit))
        results.resize(limit);
    return results;
}
