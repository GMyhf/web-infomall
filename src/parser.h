/*
 * parser.h — GB18030-family→UTF-8 converter and TenMillionArticles parser.
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

// ── GB18030-family → UTF-8 Converter ──────────────────────────

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
            errors_.push_back(std::string("Cannot open ") + filepath);
            return 0;
        }

        std::string buffer;
        buffer.reserve(CHUNK * 2);
        std::vector<char> chunk_buf(CHUNK);
        char* chunk = chunk_buf.data();
        int count = 0;

        auto emit_record = [&](std::string record) {
            // Strip leading whitespace
            size_t start = 0;
            while (start < record.size() &&
                   (record[start] == '\n' || record[start] == '\r' || record[start] == ' '))
                start++;
            if (start >= record.size()) return false;
            if (start > 0) record.erase(0, start);

            auto art = parse_record(record);
            if (art.id <= 0 || art.url.empty()) {
                errors_.push_back("Skipping malformed record (missing valid id or URL)");
                return false;
            }
            cb(art);
            count++;
            return max_articles > 0 && count >= max_articles;
        };

        while (f.read(chunk, CHUNK) || f.gcount() > 0) {
            size_t n = f.gcount();
            buffer.append(chunk, n);

            // Advance through the chunk and erase the consumed prefix once. The
            // previous per-record erase memmoved the remaining multi-megabyte
            // buffer repeatedly, making large imports needlessly quadratic.
            size_t consumed = 0;
            size_t pos;
            while ((pos = buffer.find(REC_SEP, consumed)) != std::string::npos) {
                if (emit_record(buffer.substr(consumed, pos - consumed))) return count;
                consumed = pos + 1;
            }
            if (consumed > 0) buffer.erase(0, consumed);

            // Safety: limit buffer growth
            if (buffer.size() > CHUNK * 3) {
                errors_.push_back("Buffer overflow, discarding partial data");
                buffer.clear();
            }
        }

        if (f.bad()) {
            errors_.push_back(std::string("Read error in ") + filepath);
        } else if (std::any_of(buffer.begin(), buffer.end(), [](unsigned char c) {
                       return !std::isspace(c);
                   })) {
            errors_.push_back("Discarding truncated final record (missing record separator)");
        }
        return count;
    }

    const auto& errors() const { return errors_; }

private:
    ParsedArticle parse_record(const std::string& raw);
};

#endif // PARSER_H
