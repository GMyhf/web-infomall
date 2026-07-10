/*
 * parser.cpp — Parse TenMillionArticles format, convert GB18030-family text to UTF-8.
 *
 * Input:  id=\x1etime=\x1eurl=\x1etitle=\x1ebody=\x1f  (GB18030/GBK encoding)
 * Output: ArticleRecord stream (UTF-8, uncompressed)
 */

#include "parser.h"
#include <cerrno>

// ── GB18030-family → UTF-8 Converter ──────────────────────────

GbToUtf8::GbToUtf8() {
    // GB18030 is a strict superset of GBK/GB2312 and matches the occasional
    // extended characters present in the retained sample data.
    cd_ = iconv_open("UTF-8", "GB18030");
    if (cd_ == (iconv_t)-1) {
        cd_ = iconv_open("UTF-8", "GBK");
    }
    if (cd_ == (iconv_t)-1) {
        cd_ = iconv_open("UTF-8", "GB2312"); // last resort
    }
    if (cd_ == (iconv_t)-1) {
        throw std::runtime_error("iconv_open: Chinese→UTF-8 not supported");
    }
}

GbToUtf8::~GbToUtf8() { if (cd_ != (iconv_t)-1) iconv_close(cd_); }

std::string GbToUtf8::convert(const char* data, size_t len) {
    if (len == 0) return "";
    // Reset iconv state for each conversion
    iconv(cd_, nullptr, nullptr, nullptr, nullptr);

    std::string out(len * 3 + 3, '\0');
    char* inbuf = const_cast<char*>(data);
    size_t inleft = len;
    char* outbuf = out.data();
    size_t outleft = out.size();

    auto ensure_output = [&](size_t need) {
        if (outleft >= need) return;
        size_t used = static_cast<size_t>(outbuf - out.data());
        out.resize(std::max(out.size() * 2, used + need));
        outbuf = out.data() + used;
        outleft = out.size() - used;
    };

    while (inleft > 0) {
        size_t ret = iconv(cd_, &inbuf, &inleft, &outbuf, &outleft);
        if (ret != static_cast<size_t>(-1)) break;

        if (errno == E2BIG) {
            ensure_output(4);
            continue;
        }

        // Preserve the remainder after malformed input instead of returning only
        // the successfully converted prefix. U+FFFD keeps the output valid UTF-8.
        int conversion_error = errno;
        if (conversion_error == EILSEQ || conversion_error == EINVAL) {
            ensure_output(3);
            *outbuf++ = static_cast<char>(0xEF);
            *outbuf++ = static_cast<char>(0xBF);
            *outbuf++ = static_cast<char>(0xBD);
            outleft -= 3;
            if (conversion_error == EINVAL) {
                inbuf += inleft;
                inleft = 0;
            } else {
                inbuf++;
                inleft--;
            }
            iconv(cd_, nullptr, nullptr, nullptr, nullptr);
            continue;
        }
        break;
    }
    out.resize(static_cast<size_t>(outbuf - out.data()));
    return out;
}

std::string GbToUtf8::convert(const std::string& s) {
    return convert(s.data(), s.size());
}

// ── Article Parser ────────────────────────────────────────────

ArticleParser::ParsedArticle ArticleParser::parse_record(const std::string& raw) {
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
