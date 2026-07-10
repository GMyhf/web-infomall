/*
 * indexer.cpp — Build sharded URL index files with embedded host blocks & URL pool.
 *
 * Pipeline:
 *   1. Accumulate entries + URLs in memory (per shard)
 *   2. Sort each shard by (host, url_hash, crawl_date DESC)
 *   3. Scan sorted entries → build HostBlock arrays + URL string pool
 *   4. Write final shard index files (v2 format with embedded URLs)
 */

#include "indexer.h"
#include <cstring>
#include <cerrno>
#include <unistd.h>

IndexBuilderV2::IndexBuilderV2(const std::string& index_dir) : index_dir_(index_dir) {}

void IndexBuilderV2::add_entry(const std::string& url, uint32_t crawl_date,
                                int64_t offset, uint32_t record_size, uint32_t file_seq) {
    if (!valid_archive_url(url) || !valid_crawl_date(crawl_date) ||
        record_size < ArticleRecord::HEADER_SIZE || record_size > MAX_RECORD_SIZE ||
        file_seq == 0) {
        fprintf(stderr, "WARN: invalid index entry skipped (url_len=%zu date=%u size=%u seq=%u)\n",
                url.size(), crawl_date, record_size, file_seq);
        return;
    }
    std::string host = extract_host(url);
    if (host.empty()) return;
    int sid = shard_for_host(host);

    EntryWithHost e;
    e.entry.url_hash = ::url_hash(url);
    e.entry.crawl_date = crawl_date;
    // file_offset is uint32_t; data files are capped at MAX_DAT_FILE (2GB), so
    // this fits. Guard explicitly so raising that cap can't silently corrupt the index.
    if (offset < 0 || offset > static_cast<int64_t>(UINT32_MAX)) {
        fprintf(stderr, "WARN: file_offset %lld out of uint32_t range for %s — skipping entry\n",
                (long long)offset, url.c_str());
        return;
    }
    e.entry.file_offset = static_cast<uint32_t>(offset);
    e.entry.record_size = static_cast<uint16_t>(record_size);
    e.entry.url_len = 0;       // filled in build_shard
    e.entry.url_offset = 0;    // filled in build_shard
    // reserved carries the data_NNNN.dat sequence so months that overflow the
    // 2GB file cap stay readable (0 in legacy indexes ≙ seq 1).
    e.entry.reserved = file_seq;
    e.host = host;
    e.url = url;
    shards_[sid].push_back(std::move(e));
}

bool IndexBuilderV2::load_existing() {
    size_t loaded = 0;
    bool ok = true;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        char leaf[32];
        snprintf(leaf, sizeof(leaf), "/url_%02d.idx", sid);
        std::string fname = index_dir_ + leaf;
        FILE* f = fopen(fname.c_str(), "rb");
        if (!f) continue;

        ShardFileHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
            fprintf(stderr, "ERROR: Cannot read existing shard %d\n", sid);
            ok = false;
            fclose(f);
            continue;
        }
        // Only v2 shards embed the URL pool; v1 cannot be reconstructed without
        // touching the data files, so we refuse to merge it (would lose URLs).
        if (hdr.magic != SHARD_MAGIC) {
            fprintf(stderr, "ERROR: shard %d magic 0x%08x is not v2; incremental merge refused\n",
                    sid, hdr.magic);
            ok = false;
            fclose(f);
            continue;
        }

        if (fseeko(f, 0, SEEK_END) != 0) { ok = false; fclose(f); continue; }
        off_t file_size = ftello(f);
        uint64_t expected_size = sizeof(ShardFileHeader)
            + static_cast<uint64_t>(hdr.host_count) * sizeof(HostBlock)
            + static_cast<uint64_t>(hdr.entry_count) * sizeof(UrlIndexEntry)
            + hdr.url_pool_size;
        if (file_size < 0 || expected_size != static_cast<uint64_t>(file_size) ||
            (hdr.entry_count > 0 && hdr.url_pool_size == 0) ||
            fseeko(f, sizeof(ShardFileHeader), SEEK_SET) != 0) {
            fprintf(stderr, "WARN: shard %d has an invalid layout; refusing incremental merge\n", sid);
            ok = false;
            fclose(f);
            continue;
        }

        // Skip the HostBlock array (rebuilt from scratch in build_shard).
        if (fseek(f, static_cast<long>(hdr.host_count) * sizeof(HostBlock), SEEK_CUR) != 0) {
            ok = false; fclose(f); continue;
        }
        std::vector<UrlIndexEntry> ents(hdr.entry_count);
        if (hdr.entry_count &&
            fread(ents.data(), sizeof(UrlIndexEntry), hdr.entry_count, f) != hdr.entry_count) {
            ok = false; fclose(f); continue;
        }
        std::vector<char> pool(hdr.url_pool_size);
        if (hdr.url_pool_size &&
            fread(pool.data(), 1, hdr.url_pool_size, f) != hdr.url_pool_size) {
            ok = false; fclose(f); continue;
        }
        fclose(f);

        auto& vec = shards_[sid];
        vec.reserve(vec.size() + hdr.entry_count);
        for (auto& e : ents) {
            if (!entry_in_pool(e, hdr.url_pool_size) || e.url_len == 0 ||
                e.url_len > MAX_URL_LEN) { ok = false; continue; }
            EntryWithHost ewh;
            ewh.entry = e;
            ewh.entry.url_offset = 0;   // recomputed in build_shard
            ewh.entry.url_len = 0;
            ewh.url.assign(pool.data() + e.url_offset, e.url_len);
            ewh.host = extract_host(ewh.url);
            if (!valid_archive_url(ewh.url) || !valid_crawl_date(e.crawl_date) ||
                e.url_hash != url_hash(ewh.url) ||
                ewh.host.empty() || shard_for_host(ewh.host) != sid) {
                ok = false;
                continue;
            }
            vec.push_back(std::move(ewh));
            loaded++;
        }
    }
    printf("Merged %zu existing index entries\n", loaded);
    return ok;
}

bool IndexBuilderV2::build() {
    if (mkdir(index_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: Cannot create index directory %s: %s\n",
                index_dir_.c_str(), strerror(errno));
        return false;
    }
    built_host_count_ = 0;
    built_entry_count_ = 0;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        if (shards_[sid].empty()) continue;
        printf("  Building shard %d/%d (%zu entries)... ", sid + 1, NUM_SHARDS, shards_[sid].size());
        fflush(stdout);
        if (!build_shard(sid)) return false;
        // Free
        shards_[sid].clear();
        shards_[sid].shrink_to_fit();
    }
    return true;
}

size_t IndexBuilderV2::total_entries() const {
    size_t n = 0;
    for (const auto& s : shards_) n += s.size();
    return n;
}

bool IndexBuilderV2::build_shard(int sid) {
    auto& items = shards_[sid];
    if (items.size() > UINT32_MAX) {
        fprintf(stderr, "ERROR: shard %d exceeds the v2 entry-count limit\n", sid);
        return false;
    }
    std::sort(items.begin(), items.end());

    // Drop exact duplicates that an incremental re-load can introduce (same URL,
    // same crawl date, same data-file offset). Distinct versions — differing date
    // or offset — are kept. Sort above makes identical records adjacent.
    items.erase(std::unique(items.begin(), items.end(),
        [](const EntryWithHost& a, const EntryWithHost& b) {
            return a.entry.url_hash == b.entry.url_hash &&
                   a.entry.crawl_date == b.entry.crawl_date &&
                   a.entry.file_offset == b.entry.file_offset &&
                   a.entry.record_size == b.entry.record_size &&
                   a.entry.reserved == b.entry.reserved &&
                   a.url == b.url;
        }), items.end());

    // Build URL string pool: concatenate all URLs
    std::string url_pool;
    url_pool.reserve(items.size() * 80); // avg URL ~80 bytes

    // Extract entries and set URL pool offsets
    std::vector<UrlIndexEntry> entries;
    entries.reserve(items.size());
    for (auto& it : items) {
        size_t ulen = it.url.size();
        if (ulen == 0 || ulen > MAX_URL_LEN ||
            url_pool.size() + ulen > UINT32_MAX) {
            fprintf(stderr, "ERROR: shard %d URL pool exceeds the v2 format limit\n", sid);
            return false;
        }
        it.entry.url_offset = static_cast<uint32_t>(url_pool.size());
        it.entry.url_len = static_cast<uint16_t>(ulen);
        url_pool.append(it.url.data(), ulen);
        entries.push_back(it.entry);
    }

    // Build host blocks: scan sorted entries
    std::vector<HostBlock> hosts;
    std::string cur_host;
    uint32_t first_idx = 0;

    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].host != cur_host) {
            if (!cur_host.empty() && i > first_idx) {
                HostBlock hb = {};
                strncpy(hb.host, cur_host.c_str(), HOST_HASH_LEN - 1);
                hb.first_entry = first_idx;
                hb.entry_count = static_cast<uint32_t>(i) - first_idx;
                hosts.push_back(hb);
            }
            cur_host = items[i].host;
            first_idx = static_cast<uint32_t>(i);
        }

        // Sanitize host
        if (!cur_host.empty() && cur_host.back() == '\n') {
            cur_host.pop_back();
        }
    }
    // Last host block
    if (!cur_host.empty() && items.size() > first_idx) {
        HostBlock hb = {};
        strncpy(hb.host, cur_host.c_str(), HOST_HASH_LEN - 1);
        hb.first_entry = first_idx;
        hb.entry_count = static_cast<uint32_t>(items.size()) - first_idx;
        hosts.push_back(hb);
    }

    std::sort(hosts.begin(), hosts.end(), host_cmp);
    built_host_count_ += hosts.size();
    built_entry_count_ += entries.size();

    // Write to a sibling temporary file and publish only after every byte has
    // reached the filesystem buffer successfully. This prevents a short write
    // from replacing a previously usable shard with a truncated one.
    char leaf[32];
    snprintf(leaf, sizeof(leaf), "/url_%02d.idx", sid);
    std::string fname = index_dir_ + leaf;
    std::string tmp_name = fname + ".tmp";
    FILE* f = fopen(tmp_name.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot create %s: %s\n", tmp_name.c_str(), strerror(errno));
        return false;
    }

    ShardFileHeader hdr = {};
    hdr.magic = SHARD_MAGIC;
    hdr.entry_count = static_cast<uint32_t>(entries.size());
    hdr.host_count = static_cast<uint32_t>(hosts.size());
    hdr.url_pool_size = static_cast<uint32_t>(url_pool.size());

    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1 &&
              fwrite(hosts.data(), sizeof(HostBlock), hosts.size(), f) == hosts.size() &&
              fwrite(entries.data(), sizeof(UrlIndexEntry), entries.size(), f) == entries.size() &&
              fwrite(url_pool.data(), 1, url_pool.size(), f) == url_pool.size() &&
              fflush(f) == 0 && fsync_file_descriptor(fileno(f));
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "ERROR: Short write while creating %s\n", tmp_name.c_str());
        unlink(tmp_name.c_str());
        return false;
    }
    if (rename(tmp_name.c_str(), fname.c_str()) != 0) {
        fprintf(stderr, "ERROR: Cannot publish %s: %s\n", fname.c_str(), strerror(errno));
        unlink(tmp_name.c_str());
        return false;
    }
    if (!fsync_directory_path(index_dir_)) {
        fprintf(stderr, "ERROR: Cannot sync index directory %s: %s\n",
                index_dir_.c_str(), strerror(errno));
        return false;
    }

    size_t sz = sizeof(hdr) + hosts.size() * sizeof(HostBlock)
              + entries.size() * sizeof(UrlIndexEntry) + url_pool.size();
    printf("Wrote %zu KB (hosts=%zu, url_pool=%.1f MB)\n",
           sz / 1024, hosts.size(), url_pool.size() / 1048576.0);
    return true;
}
