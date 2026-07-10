/*
 * indexer.h — Sharded URL index builder (v2 format with embedded URL pool).
 */

#ifndef INDEXER_H
#define INDEXER_H

#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>

class IndexBuilderV2 {
    std::string index_dir_;
    struct EntryWithHost {
        UrlIndexEntry entry;
        std::string host;
        std::string url;
        // The first three keys are the on-disk lookup order. The remaining
        // keys make equivalent entries deterministic and place exact
        // duplicates next to each other for std::unique().
        bool operator<(const EntryWithHost& o) const {
            int hc = host.compare(o.host);
            if (hc != 0) return hc < 0;
            if (entry.url_hash != o.entry.url_hash) return entry.url_hash < o.entry.url_hash;
            if (entry.crawl_date != o.entry.crawl_date)
                return entry.crawl_date > o.entry.crawl_date;
            if (url != o.url) return url < o.url;
            if (entry.reserved != o.entry.reserved) return entry.reserved < o.entry.reserved;
            if (entry.file_offset != o.entry.file_offset)
                return entry.file_offset < o.entry.file_offset;
            return entry.record_size < o.entry.record_size;
        }
    };
    std::vector<EntryWithHost> shards_[NUM_SHARDS];
    size_t built_host_count_ = 0;
    size_t built_entry_count_ = 0;

public:
    explicit IndexBuilderV2(const std::string& index_dir);

    void add_entry(const std::string& url, uint32_t crawl_date,
                   int64_t offset, uint32_t record_size, uint32_t file_seq = 1);

    // Incremental merge: read existing v2 shard files back into the in-memory
    // accumulator so a subsequent build() rewrites old + new entries together
    // instead of clobbering prior data. Safe to call before adding new entries.
    bool load_existing();

    bool build();

    size_t total_entries() const;
    size_t total_hosts() const { return built_host_count_; }
    size_t built_entries() const { return built_entry_count_; }

private:
    bool build_shard(int sid);
};

using Indexer = IndexBuilderV2;

#endif // INDEXER_H
