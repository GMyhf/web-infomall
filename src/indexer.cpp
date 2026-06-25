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

IndexBuilderV2::IndexBuilderV2(const std::string& index_dir) : index_dir_(index_dir) {
    for (auto& s : shards_) s.reserve(600000); // ~600K per shard for 14M total
}

void IndexBuilderV2::add_entry(const std::string& url, uint32_t crawl_date,
                                int64_t offset, uint32_t record_size) {
    std::string host = extract_host(url);
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
    e.entry.reserved = 0;
    e.host = host;
    e.url = url;
    shards_[sid].push_back(std::move(e));
}

void IndexBuilderV2::load_existing() {
    size_t loaded = 0;
    for (int sid = 0; sid < NUM_SHARDS; sid++) {
        char fname[64];
        snprintf(fname, sizeof(fname), "%s/url_%02d.idx", index_dir_.c_str(), sid);
        FILE* f = fopen(fname, "rb");
        if (!f) continue;

        ShardFileHeader hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); continue; }
        // Only v2 shards embed the URL pool; v1 cannot be reconstructed without
        // touching the data files, so we refuse to merge it (would lose URLs).
        if (hdr.magic != SHARD_MAGIC) {
            fprintf(stderr, "WARN: shard %d magic 0x%08x is not v2; cannot merge — "
                            "existing entries in this shard will be lost\n", sid, hdr.magic);
            fclose(f);
            continue;
        }

        // Skip the HostBlock array (rebuilt from scratch in build_shard).
        if (fseek(f, static_cast<long>(hdr.host_count) * sizeof(HostBlock), SEEK_CUR) != 0) {
            fclose(f); continue;
        }
        std::vector<UrlIndexEntry> ents(hdr.entry_count);
        if (hdr.entry_count &&
            fread(ents.data(), sizeof(UrlIndexEntry), hdr.entry_count, f) != hdr.entry_count) {
            fclose(f); continue;
        }
        std::vector<char> pool(hdr.url_pool_size);
        if (hdr.url_pool_size &&
            fread(pool.data(), 1, hdr.url_pool_size, f) != hdr.url_pool_size) {
            fclose(f); continue;
        }
        fclose(f);

        auto& vec = shards_[sid];
        vec.reserve(vec.size() + hdr.entry_count);
        for (auto& e : ents) {
            if (!entry_in_pool(e, hdr.url_pool_size)) continue;  // skip corrupt slice
            EntryWithHost ewh;
            ewh.entry = e;
            ewh.entry.url_offset = 0;   // recomputed in build_shard
            ewh.entry.url_len = 0;
            ewh.url.assign(pool.data() + e.url_offset, e.url_len);
            ewh.host = extract_host(ewh.url);
            vec.push_back(std::move(ewh));
            loaded++;
        }
    }
    printf("Merged %zu existing index entries\n", loaded);
}

bool IndexBuilderV2::build() {
    mkdir(index_dir_.c_str(), 0755);
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
    std::sort(items.begin(), items.end());

    // Drop exact duplicates that an incremental re-load can introduce (same URL,
    // same crawl date, same data-file offset). Distinct versions — differing date
    // or offset — are kept. Sort above makes identical records adjacent.
    items.erase(std::unique(items.begin(), items.end(),
        [](const EntryWithHost& a, const EntryWithHost& b) {
            return a.entry.url_hash == b.entry.url_hash &&
                   a.entry.crawl_date == b.entry.crawl_date &&
                   a.entry.file_offset == b.entry.file_offset &&
                   a.url == b.url;
        }), items.end());

    // Build URL string pool: concatenate all URLs
    std::string url_pool;
    url_pool.reserve(items.size() * 80); // avg URL ~80 bytes

    // Extract entries and set URL pool offsets
    std::vector<UrlIndexEntry> entries;
    entries.reserve(items.size());
    for (auto& it : items) {
        // url_len is uint16_t and lookups assume url_pool[offset .. offset+url_len)
        // is exactly the stored URL, so clamp BOTH the length field and the bytes
        // appended to the pool to the same value. MAX_URL_LEN (2048) << 65535.
        size_t ulen = it.url.size();
        if (ulen > MAX_URL_LEN) {
            fprintf(stderr, "WARN: URL length %zu exceeds MAX_URL_LEN (%u), truncating: %s\n",
                    ulen, MAX_URL_LEN, it.url.c_str());
            ulen = MAX_URL_LEN;
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

    // Write shard file
    char fname[64];
    snprintf(fname, sizeof(fname), "%s/url_%02d.idx", index_dir_.c_str(), sid);
    FILE* f = fopen(fname, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: Cannot create %s\n", fname);
        return false;
    }

    ShardFileHeader hdr = {};
    hdr.magic = SHARD_MAGIC;
    hdr.entry_count = static_cast<uint32_t>(entries.size());
    hdr.host_count = static_cast<uint32_t>(hosts.size());
    hdr.url_pool_size = static_cast<uint32_t>(url_pool.size());

    fwrite(&hdr, sizeof(hdr), 1, f);
    fwrite(hosts.data(), sizeof(HostBlock), hosts.size(), f);
    fwrite(entries.data(), sizeof(UrlIndexEntry), entries.size(), f);
    fwrite(url_pool.data(), 1, url_pool.size(), f);
    fclose(f);

    size_t sz = sizeof(hdr) + hosts.size() * sizeof(HostBlock)
              + entries.size() * sizeof(UrlIndexEntry) + url_pool.size();
    printf("Wrote %zu KB (hosts=%zu, url_pool=%.1f MB)\n",
           sz / 1024, hosts.size(), url_pool.size() / 1048576.0);
    return true;
}
