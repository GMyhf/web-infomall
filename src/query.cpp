/*
 * query.cpp — QueryEngine: URL lookup, host search, prefix search.
 *
 * Uses mmap'd shard index files for zero-copy binary search.
 * All lookups are O(log N).  v2 shards embed URLs directly in the
 * index file (url_pool), eliminating data-file IO for searches.
 */

#include "query.h"

// ── MappedShard ────────────────────────────────────────────────

bool MappedShard::open(const char* path) {
    fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); fd = -1; return false; }
    file_size = st.st_size;

    data = mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); fd = -1; return false; }

    header = static_cast<const ShardFileHeader*>(data);
    if (header->magic != SHARD_MAGIC && header->magic != SHARD_MAGIC_V1) {
        munmap(data, file_size); close(fd); fd = -1; data = nullptr;
        return false;
    }
    is_v2 = (header->magic == SHARD_MAGIC);
    hosts = reinterpret_cast<const HostBlock*>(header + 1);
    entries = reinterpret_cast<const UrlIndexEntry*>(hosts + header->host_count);

    // v2: URL pool follows entries array
    if (is_v2 && header->url_pool_size > 0) {
        url_pool = reinterpret_cast<const char*>(entries + header->entry_count);
    }
    return true;
}

MappedShard::~MappedShard() {
    if (data && data != MAP_FAILED) munmap(data, file_size);
    if (fd >= 0) close(fd);
}

// ── ArticleReader ──────────────────────────────────────────────

ArticleReader::ArticleReader(const std::string& data_dir) : data_dir_(data_dir) {}
ArticleReader::~ArticleReader() { for (auto& kv : open_files_) fclose(kv.second); }

FILE* ArticleReader::open_file(const std::string& rel_path) {
    auto it = open_files_.find(rel_path);
    if (it != open_files_.end()) return it->second;
    std::string full = data_dir_ + "/" + rel_path;
    FILE* f = fopen(full.c_str(), "rb");
    if (f) open_files_[rel_path] = f;
    return f;
}

ArticleReader::Article ArticleReader::read_article(
    const std::string& rel_path, int64_t offset, uint32_t size) {
    Article art = {};
    if (size < ArticleRecord::HEADER_SIZE) return art;

    std::vector<char> buf(size);
    FILE* f = open_file(rel_path);
    if (!f) return art;
    fseeko(f, offset, SEEK_SET);
    if (fread(buf.data(), 1, size, f) != size) return art;

    auto* rec = reinterpret_cast<const ArticleRecord*>(buf.data());
    if (rec->magic != ARTICLE_MAGIC) return art;

    // Verify CRC-32 if present (non-zero means it was computed)
    if (rec->crc32 != 0) {
        uint32_t expected = compute_record_crc32(rec);
        if (expected != rec->crc32) {
            fprintf(stderr, "CRC32 MISMATCH at offset %lld in %s: got 0x%08x, expected 0x%08x\n",
                    (long long)offset, rel_path.c_str(), rec->crc32, expected);
            // Surface corruption to the caller. Data is still populated below
            // so integrity tooling can inspect it, but the page is not served.
            art.valid = false;
        }
    }

    art.url.assign(rec->url(), rec->url_len);
    art.title.assign(rec->title(), rec->title_len);
    art.date = rec->crawl_date;

    bool compressed = (rec->flags & 1);
    if (compressed && rec->body_compr_len > 0) {
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
            art.valid = false;
        }
    } else {
        art.body.assign(rec->body(), rec->body_compr_len);
    }
    return art;
}

// ── Query Engine ──────────────────────────────────────────────

std::string QueryEngine::data_path(uint32_t crawl_date) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%04u%02u/data_0001.dat",
             crawl_date / 10000, (crawl_date / 100) % 100);
    return buf;
}

QueryEngine::QueryEngine(const std::string& data_dir, const std::string& index_dir)
    : data_dir_(data_dir), index_dir_(index_dir), reader_(data_dir) {}

bool QueryEngine::init() {
    srand(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()));
    for (int i = 0; i < NUM_SHARDS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/url_%02d.idx", index_dir_.c_str(), i);
        if (!shards_[i].open(path)) continue;
        shards_loaded_++;
    }
    printf("Loaded %d/%d shard index files\n", shards_loaded_, NUM_SHARDS);

    // Load precomputed auxiliary data
    load_year_dist();
    load_today();
    load_title_index();

    return shards_loaded_ > 0;
}

// ── Precomputed data loading ──────────────────────────────────

void QueryEngine::load_year_dist() {
    char path[256];
    snprintf(path, sizeof(path), "%s/year_dist.dat", index_dir_.c_str());
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint32_t count;
    if (fread(&count, sizeof(count), 1, f) != 1) { fclose(f); return; }
    year_dist_cached_.clear();
    year_dist_cached_.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t year, cnt;
        if (fread(&year, sizeof(year), 1, f) != 1) break;
        if (fread(&cnt, sizeof(cnt), 1, f) != 1) break;
        year_dist_cached_.push_back({year, cnt});
    }
    fclose(f);
    printf("  Loaded %zu year distribution entries\n", year_dist_cached_.size());
}

void QueryEngine::load_today() {
    char path[256];
    snprintf(path, sizeof(path), "%s/today.dat", index_dir_.c_str());
    FILE* f = fopen(path, "rb");
    if (!f) return;
    uint32_t num_days;
    if (fread(&num_days, sizeof(num_days), 1, f) != 1) { fclose(f); return; }
    today_data_.clear();
    today_data_.reserve(num_days);
    for (uint32_t i = 0; i < num_days; i++) {
        TodayEntry te;
        if (fread(&te.mmdd, sizeof(te.mmdd), 1, f) != 1) break;
        uint32_t url_count;
        if (fread(&url_count, sizeof(url_count), 1, f) != 1) break;
        te.urls.reserve(url_count);
        for (uint32_t j = 0; j < url_count; j++) {
            uint16_t len;
            if (fread(&len, sizeof(len), 1, f) != 1) break;
            std::string url(len, '\0');
            if (fread(&url[0], 1, len, f) != len) break;
            te.urls.push_back(std::move(url));
        }
        today_data_.push_back(std::move(te));
    }
    fclose(f);
    printf("  Loaded %zu today-in-history entries\n", today_data_.size());
}

// ── URL resolve helpers ──────────────────────────────────────

std::string QueryEngine::get_entry_url(const UrlIndexEntry& ent, int sid) {
    auto& shard = shards_[sid];
    if (shard.is_v2 && shard.url_pool) {
        return entry_url(ent, shard.url_pool, shard.header->url_pool_size);
    }
    // v1 fallback: read from data file
    auto buf = reader_.read_article(data_path(ent.crawl_date),
                                     ent.file_offset, ent.record_size);
    return buf.url;
}

// ── Single-page lookup ───────────────────────────────────────

ArticleReader::Article QueryEngine::get_page(const std::string& url, uint32_t date) {
    std::string host = extract_host(url);
    int sid = shard_for_host(host);
    if (sid < 0 || sid >= NUM_SHARDS) return {};

    auto& shard = shards_[sid];
    if (!shard.data) return {};

    auto* hb = find_host(shard.hosts, shard.header->host_count, host);
    if (!hb) return {};

    uint64_t hash = ::url_hash(url);
    const UrlIndexEntry* block = shard.entries + hb->first_entry;
    auto* first = find_first(block, hb->entry_count, hash);
    if (!first) return {};

    // Count versions and find best match
    uint32_t idx = static_cast<uint32_t>(first - block);
    uint32_t count = 0;
    while (idx + count < hb->entry_count && block[idx + count].url_hash == hash) count++;

    uint32_t best_idx = idx;
    if (date > 0 && count > 1) {
        uint32_t best_diff = UINT32_MAX;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t d = block[idx + i].crawl_date;
            uint32_t diff = (d > date) ? (d - date) : (date - d);
            if (diff < best_diff) { best_diff = diff; best_idx = idx + i; }
        }
    }

    auto& ent = block[best_idx];
    return reader_.read_article(data_path(ent.crawl_date), ent.file_offset, ent.record_size);
}

// ── Version listing ──────────────────────────────────────────

std::vector<QueryEngine::Version> QueryEngine::get_versions(const std::string& url) {
    std::vector<Version> vers;
    std::string host = extract_host(url);
    int sid = shard_for_host(host);
    if (sid < 0 || sid >= NUM_SHARDS) return vers;

    auto& shard = shards_[sid];
    if (!shard.data) return vers;

    auto* hb = find_host(shard.hosts, shard.header->host_count, host);
    if (!hb) return vers;

    uint64_t hash = ::url_hash(url);
    const UrlIndexEntry* block = shard.entries + hb->first_entry;
    auto* first = find_first(block, hb->entry_count, hash);
    if (!first) return vers;

    uint32_t idx = static_cast<uint32_t>(first - block);
    while (idx < hb->entry_count && block[idx].url_hash == hash) {
        Version v;
        v.date = block[idx].crawl_date;
        uint32_t count = 1;
        while (idx + count < hb->entry_count &&
               block[idx + count].url_hash == hash &&
               block[idx + count].crawl_date == v.date) count++;
        v.record_count = count;
        vers.push_back(v);
        idx += count;
    }
    return vers;
}

// ── Host search (substring) ──────────────────────────────────

std::vector<std::pair<std::string, uint32_t>> QueryEngine::search_host_substring(
        const std::string& substr, int limit) {
    std::vector<std::pair<std::string, uint32_t>> results;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;
        for (uint32_t i = 0; i < shard.header->host_count &&
             results.size() < static_cast<size_t>(limit); i++) {
            std::string host_name(shard.hosts[i].host,
                strnlen(shard.hosts[i].host, HOST_HASH_LEN));
            if (host_name.find(substr) != std::string::npos) {
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
    int sid = shard_for_host(host);
    if (sid < 0 || sid >= NUM_SHARDS) return urls;

    auto& shard = shards_[sid];
    if (!shard.data) return urls;

    auto* hb = find_host(shard.hosts, shard.header->host_count, host);
    if (!hb) return urls;

    const UrlIndexEntry* block = shard.entries + hb->first_entry;
    std::string last_url;

    for (uint32_t i = 0; i < hb->entry_count && urls.size() < static_cast<size_t>(limit); i++) {
        auto& ent = block[i];
        std::string url;
        if (shard.is_v2 && shard.url_pool) {
            url = entry_url(ent, shard.url_pool, shard.header->url_pool_size);
        } else {
            // v1 fallback
            url = reader_.read_article(data_path(ent.crawl_date),
                                        ent.file_offset,
                                        std::min<uint32_t>(ent.record_size,
                                                 ArticleRecord::HEADER_SIZE + 1024u)).url;
        }
        if (!url.empty() && url != last_url) {
            urls.push_back({url, ent.crawl_date});
            last_url = url;
        }
    }
    return urls;
}

// ── URL prefix search ────────────────────────────────────────

std::vector<std::string> QueryEngine::search_prefix(const std::string& prefix, int limit) {
    std::vector<std::string> urls;

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
                auto art = reader_.read_article(data_path(ent.crawl_date),
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
            if (total_urls > 0) return;
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
        if (shard.header->entry_count > 0) {
            uint32_t d = shard.entries[0].crawl_date;
            if (d < date_min) date_min = d;
            d = shard.entries[shard.header->entry_count - 1].crawl_date;
            if (d > date_max) date_max = d;
        }
    }
}

// ── Top hosts ────────────────────────────────────────────────

std::vector<std::pair<std::string, uint32_t>> QueryEngine::get_top_hosts(int limit) {
    std::vector<std::pair<std::string, uint32_t>> hosts;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        auto& shard = shards_[sid];
        if (!shard.data) continue;
        for (uint32_t i = 0; i < shard.header->host_count; i++) {
            std::string name(shard.hosts[i].host, strnlen(shard.hosts[i].host, HOST_HASH_LEN));
            hosts.emplace_back(name, shard.hosts[i].entry_count);
        }
    }
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
    std::vector<int> valid_shards;
    for (int sid = 0; sid < NUM_SHARDS; sid++)
        if (shards_[sid].data && shards_[sid].header->entry_count > 0)
            valid_shards.push_back(sid);
    if (valid_shards.empty()) return "";

    int sid = valid_shards[rand() % valid_shards.size()];
    auto& shard = shards_[sid];
    uint32_t idx = rand() % shard.header->entry_count;
    auto& ent = shard.entries[idx];

    if (shard.is_v2 && shard.url_pool)
        return entry_url(ent, shard.url_pool, shard.header->url_pool_size);
    // v1 fallback
    return reader_.read_article(data_path(ent.crawl_date),
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
    if (mmdd < 101 || mmdd > 1231) return urls;

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
                    url = reader_.read_article(data_path(ent.crawl_date),
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

    std::string last;
    for (auto& c : candidates) {
        if (c.url == last) continue;
        last = c.url;
        urls.push_back(c.url);
        if (urls.size() >= static_cast<size_t>(limit)) break;
    }
    return urls;
}

// ── Browse by date ───────────────────────────────────────────

std::vector<QueryEngine::UrlWithDate> QueryEngine::get_by_date(uint32_t date, int limit) {
    std::vector<UrlWithDate> results;
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
                    url = reader_.read_article(data_path(ent.crawl_date),
                        ent.file_offset, std::min<uint32_t>(ent.record_size,
                            ArticleRecord::HEADER_SIZE + 1024u)).url;
                }
                if (!url.empty())
                    results.push_back({url, ent.crawl_date});
            }
        }
    }
    return results;
}

// ── Get article by exact URL + date ──────────────────────────

ArticleReader::Article QueryEngine::get_page_by_date(const std::string& url, uint32_t date) {
    std::string host = extract_host(url);
    int sid = shard_for_host(host);
    if (sid < 0 || sid >= NUM_SHARDS) return {};

    auto& shard = shards_[sid];
    if (!shard.data) return {};

    auto* hb = find_host(shard.hosts, shard.header->host_count, host);
    if (!hb) return {};

    uint64_t hash = ::url_hash(url);
    const UrlIndexEntry* block = shard.entries + hb->first_entry;
    auto* first = find_first(block, hb->entry_count, hash);
    if (!first) return {};

    uint32_t idx = static_cast<uint32_t>(first - block);
    while (idx < hb->entry_count && block[idx].url_hash == hash) {
        if (block[idx].crawl_date == date) {
            auto& ent = block[idx];
            return reader_.read_article(data_path(ent.crawl_date),
                ent.file_offset, ent.record_size);
        }
        idx++;
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
    title_index_.reserve(num_terms);

    for (uint32_t i = 0; i < num_terms; i++) {
        TitleTerm tt;
        uint16_t tlen;
        if (fread(&tlen, sizeof(tlen), 1, f) != 1) break;
        tt.term.resize(tlen);
        if (fread(&tt.term[0], 1, tlen, f) != tlen) break;

        uint32_t num_posts;
        if (fread(&num_posts, sizeof(num_posts), 1, f) != 1) break;
        tt.postings.reserve(num_posts);

        for (uint32_t j = 0; j < num_posts; j++) {
            TitlePosting tp;
            uint16_t ulen, tlen2;
            if (fread(&tp.date, sizeof(tp.date), 1, f) != 1) break;
            if (fread(&ulen, sizeof(ulen), 1, f) != 1) break;
            tp.url.resize(ulen);
            if (fread(&tp.url[0], 1, ulen, f) != ulen) break;
            if (fread(&tlen2, sizeof(tlen2), 1, f) != 1) break;
            tp.title.resize(tlen2);
            if (fread(&tp.title[0], 1, tlen2, f) != tlen2) break;
            tt.postings.push_back(std::move(tp));
        }
        title_index_.push_back(std::move(tt));
    }
    fclose(f);
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
    if (title_index_.empty()) return results;

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
