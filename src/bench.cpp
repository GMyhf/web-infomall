/*
 * bench.cpp — QueryEngine benchmark tool.
 *
 * Measures lookup performance by running random queries through
 * the full get_page() path: shard index binary search + data file retrieval.
 *
 * Usage: ./bench <data_dir> <index_dir> [num_queries]
 *
 * Build:  make bench    (defined in Makefile as bench: bench.o query.o)
 */

#include "common.h"
#include "query.h"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>

// ── Percentile helper (linear interpolation) ──────────────────────────────

static double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = (p / 100.0) * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <data_dir> <index_dir> [num_queries]\n", argv[0]);
        return 1;
    }

    std::string data_dir  = argv[1];
    std::string index_dir = argv[2];
    int num_queries = (argc > 3) ? std::atoi(argv[3]) : 10000;
    if (num_queries <= 0) num_queries = 10000;

    printf("Web InfoMall -- QueryEngine Benchmark\n");
    printf("====================================\n\n");
    printf("Configuration:\n");
    printf("  Data dir:      %s\n", data_dir.c_str());
    printf("  Index dir:     %s\n", index_dir.c_str());
    printf("  Num queries:   %d\n\n", num_queries);

    // ── Init ──────────────────────────────────────────────────────────────

    printf("Initializing QueryEngine...\n");
    QueryEngine qe(data_dir, index_dir);
    if (!qe.init()) {
        fprintf(stderr, "ERROR: Failed to initialize QueryEngine.\n");
        return 1;
    }
    printf("\n");

    // ── Generate random query URLs from the index ─────────────────────────

    printf("Generating %d random query URLs...\n", num_queries);
    std::vector<std::string> urls;
    urls.reserve(static_cast<size_t>(num_queries));
    for (int i = 0; i < num_queries; i++) {
        std::string url = qe.get_random_url();
        if (!url.empty()) {
            urls.push_back(std::move(url));
        }
    }

    if (urls.empty()) {
        fprintf(stderr, "ERROR: No URLs could be generated from the index.\n");
        return 1;
    }

    printf("  Generated %zu valid URLs.\n\n", urls.size());

    // ── Warm-up (prime page cache, CPU caches) ────────────────────────────

    size_t warmup = std::min<size_t>(100, urls.size());
    printf("Warming up (%zu iterations)...\n", warmup);
    for (size_t i = 0; i < warmup; i++) {
        volatile auto _ = qe.get_page(urls[i]);
        (void)_;
    }
    printf("  Done.\n\n");

    // ── Benchmark loop ────────────────────────────────────────────────────

    printf("Benchmarking %zu queries...\n", urls.size());

    std::vector<double> latencies_us;
    latencies_us.reserve(urls.size());

    auto bench_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < urls.size(); i++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        auto result = qe.get_page(urls[i]);
        auto t1 = std::chrono::high_resolution_clock::now();
        (void)result;

        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        latencies_us.push_back(us);
    }

    auto bench_end = std::chrono::high_resolution_clock::now();
    double total_sec = std::chrono::duration<double>(bench_end - bench_start).count();

    // ── Statistics ───────────────────────────────────────────────────────

    std::vector<double> sorted = latencies_us;
    std::sort(sorted.begin(), sorted.end());

    double sum = 0;
    for (double v : sorted) sum += v;

    size_t n = sorted.size();
    double qps      = (total_sec > 0) ? static_cast<double>(n) / total_sec : 0.0;
    double avg      = sum / static_cast<double>(n);
    double min_lat  = sorted.front();
    double max_lat  = sorted.back();
    double p50      = percentile(sorted, 50.0);
    double p95      = percentile(sorted, 95.0);
    double p99      = percentile(sorted, 99.0);

    // ── Results table ────────────────────────────────────────────────────

    printf("\n");
    printf("+--------------------------------------+------------------------+\n");
    printf("| %-36s | %-22s |\n", "Metric", "Value");
    printf("+--------------------------------------+------------------------+\n");
    printf("| %-36s | %-22zu |\n", "Total Queries", n);
    printf("| %-36s | %-22.4f s |\n", "Total Time", total_sec);
    printf("| %-36s | %-22.1f |\n", "Queries/sec (QPS)", qps);
    printf("+--------------------------------------+------------------------+\n");
    printf("| %-36s | %-22.1f |\n", "Min Latency (us)", min_lat);
    printf("| %-36s | %-22.1f |\n", "Avg Latency (us)", avg);
    printf("| %-36s | %-22.1f |\n", "Max Latency (us)", max_lat);
    printf("+--------------------------------------+------------------------+\n");
    printf("| %-36s | %-22.1f |\n", "P50 Latency (us)", p50);
    printf("| %-36s | %-22.1f |\n", "P95 Latency (us)", p95);
    printf("| %-36s | %-22.1f |\n", "P99 Latency (us)", p99);
    printf("+--------------------------------------+------------------------+\n");

    return 0;
}
