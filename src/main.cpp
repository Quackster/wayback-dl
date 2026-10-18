// wayback-dl — bulk-download archived files from the Wayback Machine.
//
// How it works:
//   1. Queries the CDX API for every capture under a URL prefix:
//        https://web.archive.org/cdx/search/cdx?url=<prefix>&matchType=prefix
//   2. Picks the best capture per file (latest HTTP 200), or every
//      snapshot with --all.
//   3. Downloads raw archived bytes via
//        https://web.archive.org/web/<timestamp>id_/<original-url>
//      (the "id_" flag returns the original file, unmodified).
//   4. Saves files under <out>/ preserving the directory structure,
//      writes a manifest, and is resume-friendly: files that already
//      exist with the expected size are skipped.
//
// Example (Habbo release-6 DCRs):
//   ./wayback-dl --out ./habbo-dcr
//
// Configuration precedence: built-in defaults < config.json < CLI flags.

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Minimal JSON parser (objects, arrays, strings, numbers, booleans, null).
// Only what is needed for a flat config file; no external dependencies.
// ---------------------------------------------------------------------------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    bool is_obj() const { return type == Type::Object; }

    const Json* find(const std::string& key) const {
        for (const auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }
    std::string get_string(const std::string& key, const std::string& def) const {
        const Json* v = find(key);
        return (v && v->type == Type::String) ? v->str : def;
    }
    long long get_int(const std::string& key, long long def) const {
        const Json* v = find(key);
        return (v && v->type == Type::Number) ? static_cast<long long>(v->num) : def;
    }
    bool get_bool(const std::string& key, bool def) const {
        const Json* v = find(key);
        return (v && v->type == Type::Bool) ? v->b : def;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}
    bool parse(Json& out, std::string& err) {
        skip_ws();
        if (!parse_value(out, err)) return false;
        skip_ws();
        if (i_ != s_.size()) {
            err = "trailing characters at position " + std::to_string(i_);
            return false;
        }
        return true;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skip_ws() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++i_;
            else
                break;
        }
    }

    bool parse_value(Json& out, std::string& err) {
        if (i_ >= s_.size()) {
            err = "unexpected end of input";
            return false;
        }
        char c = s_[i_];
        if (c == '{') return parse_object(out, err);
        if (c == '[') return parse_array(out, err);
        if (c == '"') {
            out.type = Json::Type::String;
            return parse_string(out.str, err);
        }
        if (c == 't') {
            if (s_.compare(i_, 4, "true") == 0) {
                i_ += 4;
                out.type = Json::Type::Bool;
                out.b = true;
                return true;
            }
        } else if (c == 'f') {
            if (s_.compare(i_, 5, "false") == 0) {
                i_ += 5;
                out.type = Json::Type::Bool;
                out.b = false;
                return true;
            }
        } else if (c == 'n') {
            if (s_.compare(i_, 4, "null") == 0) {
                i_ += 4;
                out.type = Json::Type::Null;
                return true;
            }
        }
        if (c == 't' || c == 'f' || c == 'n') {
            err = "invalid literal at position " + std::to_string(i_);
            return false;
        }
        return parse_number(out, err);
    }

    bool parse_string(std::string& out, std::string& err) {
        ++i_; // opening quote
        out.clear();
        while (true) {
            if (i_ >= s_.size()) {
                err = "unterminated string";
                return false;
            }
            char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) {
                err = "unterminated escape sequence";
                return false;
            }
            char e = s_[i_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    auto hex4 = [&](unsigned& v) -> bool {
                        v = 0;
                        for (int k = 0; k < 4; ++k) {
                            if (i_ >= s_.size()) return false;
                            char h = s_[i_++];
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
                            else return false;
                        }
                        return true;
                    };
                    unsigned cp = 0;
                    if (!hex4(cp)) {
                        err = "bad \\u escape";
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) {
                            err = "bad \\u escape";
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp < 0x80) {
                        out += static_cast<char>(cp);
                    } else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xF0 | (cp >> 18));
                        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    err = "invalid escape character";
                    return false;
            }
        }
    }

    bool parse_number(Json& out, std::string& err) {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
        while (i_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+'))
            ++i_;
        if (i_ == start) {
            err = "invalid number at position " + std::to_string(i_);
            return false;
        }
        std::string tok = s_.substr(start, i_ - start);
        char* end = nullptr;
        double v = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str()) {
            err = "invalid number: " + tok;
            return false;
        }
        out.type = Json::Type::Number;
        out.num = v;
        return true;
    }

    bool parse_array(Json& out, std::string& err) {
        ++i_; // '['
        out.type = Json::Type::Array;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            Json item;
            if (!parse_value(item, err)) return false;
            out.arr.push_back(std::move(item));
            skip_ws();
            if (i_ >= s_.size()) {
                err = "unterminated array";
                return false;
            }
            if (s_[i_] == ',') {
                ++i_;
                skip_ws();
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            err = "expected ',' or ']' in array";
            return false;
        }
    }

    bool parse_object(Json& out, std::string& err) {
        ++i_; // '{'
        out.type = Json::Type::Object;
        skip_ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"') {
                err = "expected string key in object";
                return false;
            }
            if (!parse_string(key, err)) return false;
            skip_ws();
            if (i_ >= s_.size() || s_[i_] != ':') {
                err = "expected ':' in object";
                return false;
            }
            ++i_;
            skip_ws();
            Json value;
            if (!parse_value(value, err)) return false;
            out.obj.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (i_ >= s_.size()) {
                err = "unterminated object";
                return false;
            }
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            err = "expected ',' or '}' in object";
            return false;
        }
    }
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    std::string base_url = "http://www.habbohotel.com/dcr/release6_20040906/";
    std::string out_dir = "./wayback";
    int jobs = 4;
    long long limit = 100000;
    std::string from; // YYYYMMDD, inclusive
    std::string to;   // YYYYMMDD, inclusive
    bool all = false;    // download every snapshot, not just the latest per file
    bool force = false;  // re-download even if the file already exists
    bool dry_run = false; // list captures, do not download
    bool verbose = false;
    std::string manifest; // default: <out_dir>/manifest.tsv
    std::string user_agent = "wayback-dl/1.0";
    int sleep_ms = 0;    // polite delay before each download
    long timeout = 600;  // per-download timeout, seconds
};

static void print_usage() {
    std::cout <<
R"(wayback-dl — download archived files from the Wayback Machine (CDX API)

Usage: wayback-dl [options]

Configuration is read from config.json (if present), then overridden by
command-line flags.

Options:
  --config <file>     Config file to use (default: ./config.json)
  --url <prefix>      Base URL prefix to archive
                      (default: http://www.habbohotel.com/dcr/release6_20040906/)
  --out <dir>         Output directory (default: ./wayback)
  --jobs <n>          Number of parallel downloads (default: 4)
  --limit <n>         Max captures to fetch from CDX (default: 100000)
  --from <YYYYMMDD>   Earliest capture date (inclusive)
  --to <YYYYMMDD>     Latest capture date (inclusive)
  --all               Download every snapshot, not just the latest per file
  --force             Re-download even if the file already exists
  --dry-run           Only list what would be downloaded
  --manifest <file>   Manifest output path (default: <out>/manifest.tsv)
  --user-agent <s>    User agent string (default: wayback-dl/1.0)
  --sleep-ms <n>      Delay before each download, in ms (default: 0)
  --timeout <sec>     Per-download timeout in seconds (default: 600)
  --verbose           Print request URLs
  -h, --help          Show this help
)";
}

static bool load_config(const std::string& path, Config& cfg, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Json j;
    JsonParser p(text);
    if (!p.parse(j, err)) {
        err = path + ": " + err;
        return false;
    }
    if (!j.is_obj()) {
        err = path + ": top-level JSON value must be an object";
        return false;
    }
    cfg.base_url = j.get_string("url", cfg.base_url);
    cfg.out_dir = j.get_string("out", cfg.out_dir);
    cfg.jobs = static_cast<int>(j.get_int("jobs", cfg.jobs));
    cfg.limit = j.get_int("limit", cfg.limit);
    cfg.from = j.get_string("from", cfg.from);
    cfg.to = j.get_string("to", cfg.to);
    cfg.all = j.get_bool("all", cfg.all);
    cfg.force = j.get_bool("force", cfg.force);
    cfg.dry_run = j.get_bool("dry_run", cfg.dry_run);
    cfg.verbose = j.get_bool("verbose", cfg.verbose);
    cfg.manifest = j.get_string("manifest", cfg.manifest);
    cfg.user_agent = j.get_string("user_agent", cfg.user_agent);
    cfg.sleep_ms = static_cast<int>(j.get_int("sleep_ms", cfg.sleep_ms));
    cfg.timeout = static_cast<long>(j.get_int("timeout", cfg.timeout));
    return true;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static bool is_unreserved(unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~';
}

// Percent-encode; keep existing %XX sequences intact (avoids double encoding).
// `keep` lists extra characters that must not be encoded.
static std::string url_encode(const std::string& s, const std::string& keep = "") {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (is_unreserved(c) || keep.find(static_cast<char>(c)) != std::string::npos) {
            out += static_cast<char>(c);
        } else if (c == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += s.substr(i, 3);
            i += 2;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static std::string human_size(long long n) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 4) {
        v /= 1024.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f %s", v, units[i]);
    return buf;
}

static bool is_date8(const std::string& s) {
    if (s.size() != 8) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error; // non-empty on transport failure
    bool ok() const { return error.empty(); }
};

static size_t buffer_callback(char* data, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(data, size * nmemb);
    return size * nmemb;
}

static void configure_common(CURL* c, const std::string& ua, long timeout_s) {
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
}

static HttpResponse http_get(const std::string& url, const std::string& ua, long timeout_s) {
    HttpResponse r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl_easy_init failed";
        return r;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    configure_common(c, ua, timeout_s);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buffer_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_easy_cleanup(c);
    return r;
}

// ---------------------------------------------------------------------------
// CDX API
// ---------------------------------------------------------------------------

struct Capture {
    std::string ts;      // 14-digit timestamp
    std::string original; // original archived URL
    long status = 0;
    long long length = -1; // -1 = unknown
};

static std::vector<Capture> cdx_query(const Config& cfg) {
    std::ostringstream q;
    q << "https://web.archive.org/cdx/search/cdx"
      << "?url=" << url_encode(cfg.base_url, ":/?&=#[]@!$*+,;")
      << "&matchType=prefix"
      << "&fl=timestamp,original,statuscode,length"
      << "&limit=" << cfg.limit;
    if (!cfg.from.empty()) q << "&from=" << cfg.from;
    if (!cfg.to.empty()) q << "&to=" << cfg.to;
    std::string url = q.str();
    if (cfg.verbose) std::cout << "CDX: " << url << "\n";

    // archive.org intermittently returns 503/429; retry with backoff.
    HttpResponse r;
    for (int attempt = 1; attempt <= 5; ++attempt) {
        r = http_get(url, cfg.user_agent, 180);
        if (r.ok() && r.status == 200) break;
        bool retryable = (!r.ok()) || r.status == 503 || r.status == 502 ||
                         r.status == 429 || r.status == 500;
        if (!retryable || attempt == 5) break;
        int delay_ms = 2000 * (1 << (attempt - 1));
        std::cerr << "CDX HTTP " << r.status << (r.error.empty() ? "" : (std::string(" (") + r.error + ")"))
                  << "; retrying in " << delay_ms / 1000 << "s...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    if (!r.ok()) {
        std::cerr << "CDX request failed: " << r.error << "\n";
        return {};
    }
    if (r.status != 200) {
        std::cerr << "CDX returned HTTP " << r.status << ":\n" << r.body << "\n";
        return {};
    }

    std::vector<Capture> out;
    std::istringstream in(r.body);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        // CDX text output is space-separated; the `original` field may itself
        // contain spaces, so parse the fixed fields from both ends and rejoin
        // the middle as the URL.
        std::vector<std::string> f;
        std::istringstream ls(line);
        std::string tok;
        while (ls >> tok) f.push_back(tok);
        if (f.size() < 4) continue;
        Capture c;
        c.ts = f[0];
        try {
            c.status = std::stol(f[f.size() - 2]);
        } catch (...) {
            continue;
        }
        if (f.back() != "-") {
            try {
                c.length = std::stoll(f.back());
            } catch (...) {
            }
        }
        std::string orig;
        for (size_t i = 1; i + 3 <= f.size(); ++i) { // f[1..n-3] = original
            if (i > 1) orig += ' ';
            orig += f[i];
        }
        c.original = orig;
        bool digits = c.ts.size() == 14;
        for (char ch : c.ts)
            if (!std::isdigit(static_cast<unsigned char>(ch))) digits = false;
        if (digits && !c.original.empty())
            out.push_back(std::move(c));
    }
    if (out.size() >= static_cast<size_t>(cfg.limit))
        std::cerr << "warning: result set hit --limit " << cfg.limit
                  << "; increase it to see all captures\n";
    return out;
}

// Pick the best capture per original URL: prefer HTTP 200, then latest timestamp.
// Callers filter out non-200 results (redirects have no fetchable raw bytes).
static std::vector<Capture> select_latest(const std::vector<Capture>& caps) {
    auto rank = [](const Capture& c) { return c.status == 200 ? 1 : 0; };
    std::map<std::string, Capture> best;
    for (const auto& c : caps) {
        Capture& b = best[c.original];
        if (b.original.empty() || rank(c) > rank(b) ||
            (rank(c) == rank(b) && c.ts > b.ts))
            b = c;
    }
    std::vector<Capture> v;
    v.reserve(best.size());
    for (auto& [k, c] : best) v.push_back(c);
    std::sort(v.begin(), v.end(),
              [](const Capture& a, const Capture& b) { return a.original < b.original; });
    return v;
}

// ---------------------------------------------------------------------------
// Task construction
// ---------------------------------------------------------------------------

static std::string sanitize_segment(const std::string& s) {
    std::string out;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        out += (std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-') ? ch : '_';
    }
    std::string res;
    for (char ch : out) {
        if (ch == '_' && !res.empty() && res.back() == '_') continue;
        res += ch;
    }
    while (res.size() > 1 && res.back() == '_') res.pop_back();
    if (res.empty() || res == ".") return "_";
    return res;
}

static bool base_prefix(const std::string& base, const std::string& s, size_t& prefix_len) {
    std::string b = base;
    if (!b.empty() && b.back() == '/') b.pop_back();
    if (s.size() < b.size()) return false;
    for (size_t i = 0; i < b.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    prefix_len = b.size();
    return true;
}

struct Task {
    std::string ts;
    std::string original;
    long status = 0;
    long long expected_size = -1;
    fs::path local; // relative to out_dir
};

static Task make_task(const Capture& c, const std::string& base, bool all_mode) {
    Task t;
    t.ts = c.ts;
    t.original = c.original;
    t.status = c.status;
    t.expected_size = c.length;

    size_t prefix_len = 0;
    std::string rel;
    if (base_prefix(base, c.original, prefix_len))
        rel = c.original.substr(prefix_len);
    while (!rel.empty() && rel.front() == '/') rel = rel.substr(1);

    std::vector<std::string> segs;
    if (rel.empty()) {
        segs.push_back("index.html"); // the directory listing page itself
    } else {
        std::istringstream in(rel);
        std::string seg;
        while (std::getline(in, seg, '/'))
            if (!seg.empty()) segs.push_back(sanitize_segment(seg));
        if (segs.empty()) segs.push_back("index.html");
    }
    if (all_mode) segs.back() += "." + c.ts;

    fs::path p;
    for (const auto& s : segs) p /= s;
    t.local = p;
    return t;
}

// https://web.archive.org/web/<ts>id_/<original-url>  (raw archived bytes)
static std::string wayback_url(const std::string& ts, const std::string& original) {
    size_t p = original.find("://");
    std::string head, rest;
    if (p == std::string::npos) {
        head = "";
        rest = original;
    } else {
        head = original.substr(0, p + 3); // scheme://
        rest = original.substr(p + 3);
    }
    size_t s = rest.find('/');
    std::string host = (s == std::string::npos) ? rest : rest.substr(0, s);
    std::string path = (s == std::string::npos) ? "" : rest.substr(s);
    return "https://web.archive.org/web/" + ts + "id_/" + head + host + url_encode(path, "/");
}

// ---------------------------------------------------------------------------
// Downloads
// ---------------------------------------------------------------------------

static size_t file_callback(char* data, size_t size, size_t nmemb, void* ud) {
    auto* f = static_cast<std::ofstream*>(ud);
    size_t n = size * nmemb;
    f->write(data, n);
    return n;
}

static bool download_file(CURL* c, const std::string& url, const fs::path& tmp,
                         const std::string& ua, long timeout_s, std::string& err) {
    std::error_code ec;
    fs::remove(tmp, ec);
    std::ofstream f(tmp, std::ios::binary);
    if (!f) {
        err = "cannot open temp file";
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    configure_common(c, ua, timeout_s);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_callback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &f);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 0L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    f.close();
    if (rc != CURLE_OK) {
        err = curl_easy_strerror(rc);
        fs::remove(tmp, ec);
        return false;
    }
    if (code != 200) {
        err = "HTTP " + std::to_string(code);
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

struct CliOverrides {
    bool set_config = false;
    std::string config_path;
    bool set_url = false;
    std::string url;
    bool set_out = false;
    std::string out;
    bool set_jobs = false;
    int jobs = 0;
    bool set_limit = false;
    long long limit = 0;
    bool set_from = false;
    std::string from;
    bool set_to = false;
    std::string to;
    bool all = false;
    bool force = false;
    bool dry_run = false;
    bool set_manifest = false;
    std::string manifest;
    bool set_ua = false;
    std::string user_agent;
    bool set_sleep = false;
    int sleep_ms = 0;
    bool set_timeout = false;
    long timeout = 0;
    bool verbose = false;
};

int main(int argc, char** argv) {
    std::cout << std::unitbuf; // flush every line (redirected logs stay readable)
    CliOverrides o;
    bool help = false;

    auto need_value = [&](int& i, const char* name) -> const char* {
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << name << "\n";
            std::exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config") {
            o.set_config = true;
            o.config_path = need_value(i, a.c_str());
        } else if (a == "--url") {
            o.set_url = true;
            o.url = need_value(i, a.c_str());
        } else if (a == "--out") {
            o.set_out = true;
            o.out = need_value(i, a.c_str());
        } else if (a == "--jobs") {
            o.set_jobs = true;
            o.jobs = std::stoi(need_value(i, a.c_str()));
        } else if (a == "--limit") {
            o.set_limit = true;
            o.limit = std::stoll(need_value(i, a.c_str()));
        } else if (a == "--from") {
            o.set_from = true;
            o.from = need_value(i, a.c_str());
        } else if (a == "--to") {
            o.set_to = true;
            o.to = need_value(i, a.c_str());
        } else if (a == "--all") {
            o.all = true;
        } else if (a == "--force") {
            o.force = true;
        } else if (a == "--dry-run") {
            o.dry_run = true;
        } else if (a == "--manifest") {
            o.set_manifest = true;
            o.manifest = need_value(i, a.c_str());
        } else if (a == "--user-agent") {
            o.set_ua = true;
            o.user_agent = need_value(i, a.c_str());
        } else if (a == "--sleep-ms") {
            o.set_sleep = true;
            o.sleep_ms = std::stoi(need_value(i, a.c_str()));
        } else if (a == "--timeout") {
            o.set_timeout = true;
            o.timeout = std::stol(need_value(i, a.c_str()));
        } else if (a == "--verbose") {
            o.verbose = true;
        } else if (a == "-h" || a == "--help") {
            help = true;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            print_usage();
            return 2;
        }
    }

    if (help) {
        print_usage();
        return 0;
    }

    // Precedence: defaults < config.json < CLI flags.
    Config cfg;
    std::string config_path = o.set_config ? o.config_path : "config.json";
    bool explicit_config = o.set_config;
    std::error_code ec;
    if (explicit_config || fs::exists(config_path, ec)) {
        if (!fs::exists(config_path, ec)) {
            std::cerr << "config file not found: " << config_path << "\n";
            return 2;
        }
        std::string err;
        if (!load_config(config_path, cfg, err)) {
            std::cerr << "config error: " << err << "\n";
            return 2;
        }
        std::cout << "Loaded config from " << config_path << "\n";
    }

    if (o.set_url) cfg.base_url = o.url;
    if (o.set_out) cfg.out_dir = o.out;
    if (o.set_jobs) cfg.jobs = o.jobs;
    if (o.set_limit) cfg.limit = o.limit;
    if (o.set_from) cfg.from = o.from;
    if (o.set_to) cfg.to = o.to;
    if (o.all) cfg.all = true;
    if (o.force) cfg.force = true;
    if (o.dry_run) cfg.dry_run = true;
    if (o.set_manifest) cfg.manifest = o.manifest;
    if (o.set_ua) cfg.user_agent = o.user_agent;
    if (o.set_sleep) cfg.sleep_ms = o.sleep_ms;
    if (o.set_timeout) cfg.timeout = o.timeout;
    if (o.verbose) cfg.verbose = true;

    if (cfg.jobs < 1) cfg.jobs = 1;
    if (!cfg.from.empty() && !is_date8(cfg.from)) {
        std::cerr << "--from must be YYYYMMDD\n";
        return 2;
    }
    if (!cfg.to.empty() && !is_date8(cfg.to)) {
        std::cerr << "--to must be YYYYMMDD\n";
        return 2;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // --- 1. Query CDX -------------------------------------------------
    std::cout << "Querying CDX for prefix: " << cfg.base_url << "\n";
    std::vector<Capture> caps = cdx_query(cfg);
    if (caps.empty()) {
        std::cerr << "No captures found for prefix (check the URL, or use --from/--to).\n";
        curl_global_cleanup();
        return 3;
    }
    std::cout << "Found " << caps.size() << " captures.\n";

    // --- 2. Build task list -------------------------------------------
    std::vector<Task> tasks;
    if (cfg.all) {
        for (const auto& c : caps) {
            size_t pl = 0;
            if (!base_prefix(cfg.base_url, c.original, pl)) continue;
            tasks.push_back(make_task(c, cfg.base_url, true));
        }
    } else {
        size_t no_200 = 0;
        for (const auto& c : select_latest(caps)) {
            size_t pl = 0;
            if (!base_prefix(cfg.base_url, c.original, pl)) continue;
            if (c.status != 200) {
                // A 3xx/4xx-only URL has no raw bytes to fetch (the id_ endpoint
                // 404s for redirects); the 200 capture of the canonical URL is
                // what we want.
                ++no_200;
                continue;
            }
            tasks.push_back(make_task(c, cfg.base_url, false));
        }
        if (no_200 > 0)
            std::cerr << "warning: " << no_200
                      << " URL(s) had no HTTP 200 capture; skipped\n";
    }
    std::cout << "Selected " << tasks.size() << " file(s) to fetch.\n";

    if (cfg.dry_run) {
        for (const auto& t : tasks) {
            std::cout << t.ts << '\t' << t.original << '\t'
                      << (t.expected_size >= 0 ? human_size(t.expected_size) : std::string("?"))
                      << '\t' << t.local.string() << "\n";
        }
        curl_global_cleanup();
        return 0;
    }

    fs::create_directories(cfg.out_dir, ec);
    struct Record {
        std::string ts, original, local, result, error;
        long long size = 0;
    };
    std::vector<Record> records;
    std::mutex rec_mtx, out_mtx, queue_mtx;
    std::atomic<size_t> next{0};
    std::atomic<long long> n_ok{0}, n_skip{0}, n_fail{0}, bytes{0};
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::jthread> pool;
    pool.reserve(cfg.jobs);
    for (int w = 0; w < cfg.jobs; ++w) {
        pool.emplace_back([&]() {
            CURL* c = curl_easy_init();
            if (!c) {
                std::lock_guard lk(out_mtx);
                std::cerr << "worker: curl_easy_init failed\n";
                return;
            }
            std::mt19937 rng(std::random_device{}());
            while (true) {
                size_t idx;
                {
                    // All tasks are known up front, so no condition variable is
                    // needed: just take the next index under the lock.
                    std::unique_lock lk(queue_mtx);
                    if (next >= tasks.size()) break;
                    idx = next++;
                }
                const Task& t = tasks[idx];
                fs::path target = fs::path(cfg.out_dir) / t.local;
                ec.clear();
                fs::create_directories(target.parent_path(), ec);

                enum class St { Ok, Skip, Fail };
                St st;
                std::string err;
                long long size = 0;

                bool skip = false;
                if (!cfg.force && fs::exists(target, ec) && !ec) {
                    // Note: the CDX `length` field does not always match the
                    // size of the id_ delivery, so existence (not size) is the
                    // skip criterion; use --force to re-fetch.
                    skip = true;
                    ec.clear();
                    size = static_cast<long long>(fs::file_size(target, ec));
                }

                if (skip) {
                    st = St::Skip;
                    ec.clear();
                    size = static_cast<long long>(fs::file_size(target, ec));
                } else {
                    if (cfg.sleep_ms > 0)
                        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.sleep_ms));
                    bool ok = false;
                    for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
                        fs::path tmp = target;
                        tmp += ".part";
                        std::string url = wayback_url(t.ts, t.original);
                        if (cfg.verbose) {
                            std::lock_guard lk(out_mtx);
                            std::cout << "  GET " << url << "\n";
                        }
                        if (download_file(c, url, tmp, cfg.user_agent, cfg.timeout, err)) {
                            ec.clear();
                            fs::rename(tmp, target, ec);
                            if (!ec) ok = true;
                            else err = "rename: " + ec.message();
                        }
                        if (!ok && attempt < 3) {
                            std::uniform_int_distribution<int> jitter(0, 500);
                            std::this_thread::sleep_for(std::chrono::milliseconds(
                                1000 * (1 << (attempt - 1)) + jitter(rng)));
                        }
                    }
                    st = ok ? St::Ok : St::Fail;
                    if (ok) {
                        ec.clear();
                        size = static_cast<long long>(fs::file_size(target, ec));
                    }
                }

                if (st == St::Ok) {
                    n_ok++;
                    bytes += size;
                } else if (st == St::Skip) {
                    n_skip++;
                } else {
                    n_fail++;
                }
                {
                    std::lock_guard lk(rec_mtx);
                    records.push_back(Record{t.ts, t.original, t.local.string(),
                                           st == St::Ok ? "ok" : (st == St::Skip ? "skip" : "fail"),
                                           err, size});
                }
                {
                    std::lock_guard lk(out_mtx);
                    std::cout << '[' << (n_ok.load() + n_skip.load() + n_fail.load()) << '/'
                              << tasks.size() << "] "
                              << (st == St::Ok ? "OK  " : st == St::Skip ? "SKIP" : "FAIL")
                              << ' ' << t.local.string()
                              << (st == St::Fail ? "  (" + err + ")" : "")
                              << '\n';
                }
            }
            curl_easy_cleanup(c);
        });
    }
    for (auto& j : pool) j.join();

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - t0).count();

    // --- 4. Summary + manifest ------------------------------------------
    std::cout << "\nDone in " << elapsed << "s: "
              << n_ok.load() << " downloaded, " << n_skip.load() << " skipped, "
              << n_fail.load() << " failed, " << human_size(bytes.load()) << " fetched.\n";

    std::string mpath = cfg.manifest.empty() ? cfg.out_dir + "/manifest.tsv" : cfg.manifest;
    fs::create_directories(fs::path(mpath).parent_path(), ec);
    std::ofstream mf(mpath);
    if (mf) {
        mf << "result\ttimestamp\tsize\toriginal\tlocal\n";
        for (const auto& r : records)
            mf << r.result << '\t' << r.ts << '\t' << r.size << '\t'
               << r.original << '\t' << r.local << '\n';
        std::cout << "Manifest written to " << mpath << "\n";
    } else {
        std::cerr << "warning: could not write manifest " << mpath << "\n";
    }

    curl_global_cleanup();
    return n_fail.load() > 0 ? 1 : 0;
}
