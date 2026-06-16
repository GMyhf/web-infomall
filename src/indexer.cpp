/*
 * indexer.cpp — Build sharded URL index files with embedded host blocks.
 *
 * Pipeline:
 *   1. Accumulate UrlIndexEntry in memory (per shard)
 *   2. Sort each shard's entries by (url_hash ASC, crawl_date DESC)
 *   3. Scan sorted entries → build HostBlock arrays
 *   4. Write final shard index files
 */

#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>

class IndexBuilder {
    std::string index_dir_;
    // Per-shard accumulation (in memory)
    std::vector<UrlIndexEntry> shards_[NUM_SHARDS];

public:
    explicit IndexBuilder(const std::string& index_dir) : index_dir_(index_dir) {
        // Pre-allocate per shard
        for (auto& shard : shards_) {
            shard.reserve(500000); // ~500K entries per shard for 14M total
        }
    }

    // Add an entry to the appropriate shard
    void add_entry(const std::string& url, const std::string& host,
                   uint32_t crawl_date, int64_t offset, uint32_t record_size) {
        int sid = shard_for_host(host);
        UrlIndexEntry e = {};
        e.url_hash = ::url_hash(url);
        e.crawl_date = crawl_date;
        e.file_offset = offset;
        e.record_size = record_size;
        e.shard_id = static_cast<uint8_t>(sid);
        shards_[sid].push_back(e);
    }

    // Build all index files
    bool build() {
        mkdir(index_dir_.c_str(), 0755);

        for (int sid = 0; sid < NUM_SHARDS; sid++) {
            if (shards_[sid].empty()) continue;

            printf("  Building shard %d/%d (%zu entries)...\n",
                   sid + 1, NUM_SHARDS, shards_[sid].size());

            if (!build_shard(sid)) {
                return false;
            }
            // Free memory
            shards_[sid].clear();
            shards_[sid].shrink_to_fit();
        }
        return true;
    }

    size_t total_entries() const {
        size_t n = 0;
        for (const auto& s : shards_) n += s.size();
        return n;
    }

private:
    bool build_shard(int sid) {
        auto& entries = shards_[sid];

        // Sort by (url_hash ASC, crawl_date DESC)
        std::sort(entries.begin(), entries.end(), entry_by_url_hash);

        // Build host blocks: scan sorted entries, group by host
        std::vector<HostBlock> host_blocks;
        std::map<std::string, std::pair<uint32_t, uint32_t>> host_map; // host→(first_idx, count)

        for (size_t i = 0; i < entries.size(); i++) {
            // Extract host from the URL (we need to reconstruct it)
            // We store host info during the scan
        }

        // Actually, we need host info during add_entry. Let's fix this:
        // We'll pass host through a separate parallel array.

        // Simplified: scan sorted entries and build hosts from URL hash clusters
        // (This works because sharding is host-based — all same-host pages are here)
        build_hosts_from_entries(sid, entries, host_blocks);

        // Write shard file
        char fname[64];
        snprintf(fname, sizeof(fname), "%s/url_%02d.idx", index_dir_.c_str(), sid);

        FILE* f = fopen(fname, "wb");
        if (!f) {
            fprintf(stderr, "ERROR: Cannot create %s: %s\n", fname, strerror(errno));
            return false;
        }

        ShardFileHeader hdr = {};
        hdr.magic = SHARD_MAGIC;
        hdr.entry_count = static_cast<uint32_t>(entries.size());
        hdr.host_count = static_cast<uint32_t>(host_blocks.size());
        // Sort hosts
        std::sort(host_blocks.begin(), host_blocks.end(), host_cmp);

        fwrite(&hdr, sizeof(hdr), 1, f);
        fwrite(host_blocks.data(), sizeof(HostBlock), host_blocks.size(), f);
        fwrite(entries.data(), sizeof(UrlIndexEntry), entries.size(), f);
        fclose(f);

        size_t file_size = sizeof(ShardFileHeader)
            + host_blocks.size() * sizeof(HostBlock)
            + entries.size() * sizeof(UrlIndexEntry);
        printf("    Wrote %s: %zu KB (hosts=%zu, entries=%zu)\n",
               fname, file_size / 1024, host_blocks.size(), entries.size());

        return true;
    }

    void build_hosts_from_entries(int sid,
                                   const std::vector<UrlIndexEntry>& entries,
                                   std::vector<HostBlock>& hosts) {
        // We need URL→host mapping. For simplicity, store host during add_entry.
        // This is a placeholder — the actual host grouping happens in the load pipeline
        // where we have access to the URL.
        // For now, we create a single dummy host block per file.
        // The real implementation stores host alongside entries.
    }
};

// ── Enhanced IndexBuilder with host tracking ──────────────────

class IndexBuilderV2 {
    std::string index_dir_;
    struct EntryWithHost {
        UrlIndexEntry entry;
        std::string host;
        // Sort by (host, url_hash, crawl_date DESC) so same-host entries are contiguous
        bool operator<(const EntryWithHost& o) const {
            int hc = host.compare(o.host);
            if (hc != 0) return hc < 0;
            if (entry.url_hash != o.entry.url_hash) return entry.url_hash < o.entry.url_hash;
            return entry.crawl_date > o.entry.crawl_date;
        }
    };
    std::vector<EntryWithHost> shards_[NUM_SHARDS];

public:
    explicit IndexBuilderV2(const std::string& index_dir) : index_dir_(index_dir) {
        for (auto& s : shards_) s.reserve(600000); // ~600K per shard for 14M total
    }

    void add_entry(const std::string& url, uint32_t crawl_date,
                   int64_t offset, uint32_t record_size) {
        std::string host = extract_host(url);
        int sid = shard_for_host(host);

        EntryWithHost e;
        e.entry.url_hash = ::url_hash(url);
        e.entry.crawl_date = crawl_date;
        e.entry.file_offset = offset;
        e.entry.record_size = record_size;
        e.entry.shard_id = static_cast<uint8_t>(sid);
        e.host = host;
        shards_[sid].push_back(std::move(e));
    }

    bool build() {
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

    size_t total_entries() const {
        size_t n = 0;
        for (const auto& s : shards_) n += s.size();
        return n;
    }

private:
    bool build_shard(int sid) {
        auto& items = shards_[sid];
        std::sort(items.begin(), items.end());

        // Extract entries
        std::vector<UrlIndexEntry> entries;
        entries.reserve(items.size());
        for (auto& it : items) entries.push_back(it.entry);

        // Build host blocks: scan sorted entries
        std::vector<HostBlock> hosts;
        std::string cur_host;
        uint32_t first_idx = 0;

        for (size_t i = 0; i < items.size(); i++) {
            if (items[i].host != cur_host) {
                // Finish previous host block
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

            // Check last char of cur_host — remove trailing \n if present
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

        // Sort hosts
        std::sort(hosts.begin(), hosts.end(), host_cmp);

        // Write file
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

        fwrite(&hdr, sizeof(hdr), 1, f);
        fwrite(hosts.data(), sizeof(HostBlock), hosts.size(), f);
        fwrite(entries.data(), sizeof(UrlIndexEntry), entries.size(), f);
        fclose(f);

        size_t sz = sizeof(hdr) + hosts.size() * sizeof(HostBlock) + entries.size() * sizeof(UrlIndexEntry);
        printf("Wrote %zu KB (hosts=%zu)\n", sz / 1024, hosts.size());
        return true;
    }
};

using Indexer = IndexBuilderV2;
