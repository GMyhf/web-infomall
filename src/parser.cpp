/*
 * parser.cpp — Parse TenMillionArticles format, convert GB2312 → UTF-8.
 *
 * Input:  id=\x1etime=\x1eurl=\x1etitle=\x1ebody=\x1f  (GB2312 encoding)
 * Output: ArticleRecord stream (UTF-8, uncompressed)
 */

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
    GbToUtf8() {
        // GBK is superset of GB2312 — handles all common Chinese encodings
        cd_ = iconv_open("UTF-8", "GBK");
        if (cd_ == (iconv_t)-1) {
            // Try GB18030 as fallback (even larger charset)
            cd_ = iconv_open("UTF-8", "GB18030");
        }
        if (cd_ == (iconv_t)-1) {
            cd_ = iconv_open("UTF-8", "GB2312"); // last resort
        }
        if (cd_ == (iconv_t)-1) {
            throw std::runtime_error("iconv_open: Chinese→UTF-8 not supported");
        }
    }
    ~GbToUtf8() { if (cd_ != (iconv_t)-1) iconv_close(cd_); }

    std::string convert(const char* data, size_t len) {
        if (len == 0) return "";
        // Reset iconv state for each conversion
        iconv(cd_, nullptr, nullptr, nullptr, nullptr);

        std::string out;
        out.resize(len * 3 + 1);
        char* inbuf = const_cast<char*>(data);
        size_t inleft = len;
        char* outbuf = &out[0];
        size_t outleft = out.size();

        size_t ret = iconv(cd_, &inbuf, &inleft, &outbuf, &outleft);
        if (ret == (size_t)-1) {
            // Partial conversion might have succeeded — use what we got
            size_t converted = out.size() - outleft;
            if (converted > 0) {
                out.resize(converted);
                return out;
            }
            // Complete failure — return as-is
            return std::string(data, len);
        }
        out.resize(out.size() - outleft);
        return out;
    }

    std::string convert(const std::string& s) {
        return convert(s.data(), s.size());
    }
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
    ParsedArticle parse_record(const std::string& raw) {
        ParsedArticle art = {};
        art.id = 0;

        size_t pos = 0;
        while (pos < raw.size()) {
            size_t sep = raw.find(LINE_SEP, pos);
            if (sep == std::string::npos) sep = raw.size();
            std::string line = raw.substr(pos, sep - pos);
            pos = sep + 1;

            // Trim leading \n\r
            size_t start = 0;
            while (start < line.size() && (line[start] == '\n' || line[start] == '\r')) start++;

            if (start >= line.size()) continue;

            const char* p = line.c_str() + start;
            if (strncmp(p, "id=", 3) == 0) {
                art.id = atoi(p + 3);
            } else if (strncmp(p, "time=", 5) == 0) {
                art.time = std::string(p + 5, line.size() - start - 5);
            } else if (strncmp(p, "url=", 4) == 0) {
                art.url = std::string(p + 4, line.size() - start - 4);
            } else if (strncmp(p, "title=", 6) == 0) {
                std::string gb(p + 6, line.size() - start - 6);
                art.title = conv_.convert(gb);
            } else if (strncmp(p, "body=", 5) == 0) {
                std::string gb(p + 5, line.size() - start - 5);
                art.body = conv_.convert(gb);
            }
        }
        return art;
    }
};

// ── Simple test main ──────────────────────────────────────────
#ifdef PARSER_TEST
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <dat_file> [max_articles]\n", argv[0]);
        return 1;
    }
    int max_articles = (argc > 2) ? atoi(argv[2]) : 100;
    ArticleParser parser;
    int count = parser.parse_file(argv[1], max_articles, [](auto& art) {
        printf("#%d %s | %.80s\n", art.id, art.time.c_str(), art.url.c_str());
        printf("  Title: %.60s\n", art.title.c_str());
        printf("  Body:  %.80s...\n\n", art.body.c_str());
    });
    printf("Parsed %d articles, %zu errors\n", count, parser.errors().size());
    return 0;
}
#endif
