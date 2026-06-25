/*
 * parser.h — GB2312→UTF-8 converter and TenMillionArticles parser.
 */

#ifndef PARSER_H
#define PARSER_H

#include "common.h"
#include <iconv.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>

// ── GB2312 → UTF-8 Converter ──────────────────────────────────

class GbToUtf8 {
    iconv_t cd_;
public:
    GbToUtf8();
    ~GbToUtf8();
    std::string convert(const char* data, size_t len);
    std::string convert(const std::string& s);
};

// ── Article Parser ────────────────────────────────────────────

class ArticleParser {
    static constexpr char LINE_SEP  = '\x1e';
    static constexpr char REC_SEP   = '\x1f';
    static constexpr size_t CHUNK   = 4 * 1024 * 1024; // 4MB

    GbToUtf8 conv_;
    std::vector<std::string> errors_;

public:
    ArticleParser() = default;

    struct ParsedArticle {
        int id;
        std::string time;    // YYYYMMDD
        std::string url;
        std::string title;
        std::string body;
    };

    // Parse a .dat file, calling callback for each article.
    // Returns number of articles parsed.
    template<typename Callback>
    int parse_file(const char* filepath, int max_articles, Callback cb) {
        std::ifstream f(filepath, std::ios::binary);
        if (!f) {
            fprintf(stderr, "ERROR: Cannot open %s\n", filepath);
            return 0;
        }

        std::string buffer;
        buffer.reserve(CHUNK * 2);
        std::vector<char> chunk_buf(CHUNK);
        char* chunk = chunk_buf.data();
        int count = 0;

        while (f.read(chunk, CHUNK) || f.gcount() > 0) {
            size_t n = f.gcount();
            buffer.append(chunk, n);

            size_t pos;
            while ((pos = buffer.find(REC_SEP)) != std::string::npos) {
                std::string record = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);

                // Strip leading whitespace
                size_t start = 0;
                while (start < record.size() && (record[start] == '\n' || record[start] == '\r' || record[start] == ' ')) start++;
                if (start >= record.size()) continue;
                if (start > 0) record = record.substr(start);

                auto art = parse_record(record);
                if (art.id > 0 && !art.url.empty()) {
                    cb(art);
                    count++;
                    if (max_articles > 0 && count >= max_articles) {
                        return count;
                    }
                }
            }

            // Safety: limit buffer growth
            if (buffer.size() > CHUNK * 3) {
                errors_.push_back("Buffer overflow, discarding partial data");
                buffer.clear();
            }
        }
        return count;
    }

    const auto& errors() const { return errors_; }

private:
    ParsedArticle parse_record(const std::string& raw);
};

#endif // PARSER_H
