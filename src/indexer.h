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
        // Sort by (host, url_hash, crawl_date DESC)
        bool operator<(const EntryWithHost& o) const {
            int hc = host.compare(o.host);
            if (hc != 0) return hc < 0;
            if (entry.url_hash != o.entry.url_hash) return entry.url_hash < o.entry.url_hash;
            return entry.crawl_date > o.entry.crawl_date;
        }
    };
    std::vector<EntryWithHost> shards_[NUM_SHARDS];

public:
    explicit IndexBuilderV2(const std::string& index_dir);

    void add_entry(const std::string& url, uint32_t crawl_date,
                   int64_t offset, uint32_t record_size);

    // Incremental merge: read existing v2 shard files back into the in-memory
    // accumulator so a subsequent build() rewrites old + new entries together
    // instead of clobbering prior data. Safe to call before adding new entries.
    void load_existing();

    bool build();

    size_t total_entries() const;

private:
    bool build_shard(int sid);
};

using Indexer = IndexBuilderV2;

#endif // INDEXER_H
