// nexon_code.cpp - TOKEN-EFFICIENT, ROBUST version
// Fixes: raw-mode tty, ANSI UI, per-tool timeouts, edit offset tracking,
//        clear error locations, no spurious post-result output, binary detect,
//        large-paste safety, multi-patch line drift fully correct.
//
// Build:  g++ -O2 -std=c++17 nexon_code.cpp -o nexon_code
// Use:    ./nexon_code /path/to/project            # interactive
//         ./nexon_code /path/to/project < req.json # piped (recommended for LLMs)

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <regex>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <algorithm>
#include <chrono>
#include <array>
#include <stdexcept>
#include <map>
#include <set>
#include <termios.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <fcntl.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace fs = std::filesystem;

// ===========================================================================
// ANSI color helpers (auto-disabled when output is not a tty)
// ===========================================================================
static bool g_colorOut = false;
static bool g_colorIn  = false;
static bool g_plainMode = false;      // --plain / TOOLS_PLAIN / NO_COLOR: no ANSI, no box-drawing decoration
static size_t g_maxOutputChars = 0;   // --max-output=N / TOOLS_MAX_OUTPUT: 0 = unlimited
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstdlib>


static size_t g_maxInputBytes = 64 * 1024 * 1024;  // --max-input=N / TOOLS_MAX_INPUT: cap on one pasted/piped JSON payload

// --- Multi-workspace registry ------------------------------------------------
// Up to a handful of registered project roots. Empty means classic
// single-root mode: every consumer below falls back to the baseDir it was
// handed, so single-root behavior is identical to before this existed.
// Populated at startup (1-3 dirs) and by the add_dir tool mid-session.
static std::vector<fs::path> g_roots;

// Which registered root owns this user-supplied path? Absolute path: the
// root that contains it. Relative path: the first root where <root>/<rel>
// actually exists. Fallback is the passed-in baseDir so single-root callers
// always get a usable root back.
static fs::path rootFor(const fs::path& baseDir, const std::string& relPath) {
    std::vector<fs::path> roots;
    if (g_roots.empty()) roots.push_back(baseDir);
    else roots = g_roots;
    fs::path rp(relPath);
    std::error_code ec;
    if (rp.is_absolute()) {
        fs::path nc = fs::weakly_canonical(rp, ec);
        if (ec) nc = rp.lexically_normal();
        for (auto& r : roots) {
            fs::path nb = fs::weakly_canonical(r, ec);
            if (ec) nb = r.lexically_normal();
            auto bi = nb.begin(), be = nb.end();
            auto ci = nc.begin(), ce = nc.end();
            bool under = true;
            for (; bi != be; ++bi, ++ci) if (ci == ce || *ci != *bi) { under = false; break; }
            if (under) return r;
        }
        return baseDir;
    }
    for (auto& r : roots) if (fs::exists(r / rp, ec)) return r;
    return baseDir;
}

// True if canon (already canonicalized) is baseDir itself or one of the
// registered extra roots — validates the optional 'dir' argument that
// sh/git/diagnostics accept to pick their working directory.
static bool isRegisteredRoot(const fs::path& baseDir, const fs::path& canon) {
    std::error_code ec;
    fs::path nb = fs::weakly_canonical(baseDir, ec);
    if (ec) nb = baseDir.lexically_normal();
    if (nb == canon) return true;
    for (auto& r : g_roots) {
        fs::path nr = fs::weakly_canonical(r, ec);
        if (ec) nr = r.lexically_normal();
        if (nr == canon) return true;
    }
    return false;
}

struct C { const char* s; };
static const C RST  {"\033[0m"};
static const C BOLD {"\033[1m"};
static const C RED  {"\033[31m"};
static const C GRN  {"\033[32m"};
static const C YEL  {"\033[33m"};
static const C MAG  {"\033[35m"};
static const C CYN  {"\033[36m"};
static const C BG_CYAN {"\033[46;30m"};
static const C BG_GRN  {"\033[42;30m"};
static const C BG_RED  {"\033[41;37m"};
static const C DIM   {"\033[2m"};

static std::ostream& operator<<(std::ostream& os, const C& c) {
    if ((&os == &std::cout && g_colorOut) || (&os == &std::cerr && g_colorOut))
        os << c.s;
    return os;
}

static fs::path g_primary;          // primary workspace, for the TUI status line
static long g_statCalls = 0;        // session tool-call counter
static std::string g_lastTool;      // last tool name, for the status line
static long g_lastMs = 0;

// --- Relay output redirection ------------------------------------------------
// When the embedded HTTP relay handles a request, printResult/printError write
// into a per-request ostringstream instead of the terminal. g_out defaults to
// std::cout so the interactive path is byte-identical to before this existed.
// The mutex serialises both output capture and command execution: nexon_code
// is not concurrency-safe on one project root, so one command at a time.
static std::ostream* g_out = &std::cout;
static std::mutex g_outMutex;

// ---- TUI helpers ------------------------------------------------------
static int termWidth() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return (int)ws.ws_col;
    return 80;
}
static std::string tuiHLine(size_t n) {
    std::string s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) s += "─";
    return s;
}
static std::string tuiCenter(const std::string& text, int width) {
    if ((int)text.size() >= width) return text;
    return std::string((size_t)((width - (int)text.size()) / 2), ' ') + text;
}
static void tuiSep() {
    std::cout << std::string((size_t)termWidth(), '-') << "\n";
}
static void tuiPrompt() {
    std::string label = g_primary.empty() ? std::string("?") : g_primary.filename().string();
    std::cout << DIM << label << " · calls: " << g_statCalls;
    if (g_statCalls > 0) std::cout << " · last: " << g_lastTool << " (" << g_lastMs << "ms)";
    std::cout << RST << "\n";
    std::cout << BOLD << CYN << "❯ " << RST;
    std::cout.flush();
}
static void tuiHeaderBox() {
    int w = termWidth();
    if (w > 60) w = 60;
    if (w < 40) w = 40;
    const size_t iw = (size_t)w - 2;
    std::cout << CYN << "┌" << tuiHLine(iw) << "┐" << RST << "\n";
    const char* rows[3] = {
        "nexon_code - local multi-tool bridge for LLMs",
        "no API keys - no subscriptions",
        "paste is all you need",
    };
    for (const char* r : rows) {
        std::string t = " " + std::string(r);
        if (t.size() > iw) t = t.substr(0, iw);
        t += std::string(iw - t.size(), ' ');
        std::cout << CYN << "│" << RST << t << CYN << "│" << RST << "\n";
    }
    std::cout << CYN << "└" << tuiHLine(iw) << "┘" << RST << "\n";
}
static void printTuiBanner() {
    if (g_colorOut) std::cout << "\033[2J\033[H";
    tuiHeaderBox();
    int w = termWidth();
    std::cout << "\n" << BOLD << tuiCenter("What can I do for you?", w) << RST << "\n";
    std::cout << tuiCenter("paste JSON - runs when braces balance", w) << "\n";
    std::cout << tuiCenter("'help' for tools - 'exit' to quit", w) << "\n";
}

// ===========================================================================
// Minimal JSON value type
// ===========================================================================
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    static Json Null_() { return Json(); }
    static Json Bool(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
    static Json Num(double v) { Json j; j.type = Type::Number; j.num = v; return j; }
    static Json Str(std::string v) { Json j; j.type = Type::String; j.str = std::move(v); return j; }
    static Json Arr() { Json j; j.type = Type::Array; return j; }
    static Json Obj() { Json j; j.type = Type::Object; return j; }

    void set(const std::string& k, Json v) {
        for (auto& p : obj) if (p.first == k) { p.second = std::move(v); return; }
        obj.emplace_back(k, std::move(v));
    }
    const Json* get(const std::string& k) const {
        for (auto& p : obj) if (p.first == k) return &p.second;
        return nullptr;
    }
    std::string getStr2(const std::string& a, const std::string& b, const std::string& def = "") const {
        auto p = get(a); if (!p) p = get(b);
        return (p && p->type == Type::String) ? p->str : def;
    }
    long getInt2(const std::string& a, const std::string& b, long def = 0) const {
        auto p = get(a); if (!p) p = get(b);
        return (p && p->type == Type::Number) ? (long)p->num : def;
    }
    bool getBool2(const std::string& a, const std::string& b, bool def = false) const {
        auto p = get(a); if (!p) p = get(b);
        return (p && p->type == Type::Bool) ? p->b : def;
    }
    const std::vector<Json>* getArr2(const std::string& a, const std::string& b) const {
        auto p = get(a); if (!p) p = get(b);
        return (p && p->type == Type::Array) ? &p->arr : nullptr;
    }

    static Json parse(const std::string& text, std::string* err = nullptr);
    std::string dump() const;
};

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    explicit JsonParser(const std::string& s_) : s(s_) {}

    void skipWs() { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
    char peek() { return i < s.size() ? s[i] : '\0'; }
    void expect(char c) {
        if (peek() != c) throw std::runtime_error(std::string("expected '") + c + "' at pos " + std::to_string(i));
        i++;
    }

    Json parseValue() {
        skipWs();
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Json::Str(parseString());
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') return parseNull();
        return parseNumber();
    }
    Json parseObject() {
        Json j = Json::Obj();
        expect('{'); skipWs();
        if (peek() == '}') { i++; return j; }
        while (true) {
            skipWs();
            std::string key = parseString();
            skipWs(); expect(':');
            Json val = parseValue();
            j.obj.emplace_back(std::move(key), std::move(val));
            skipWs();
            if (peek() == ',') { i++; continue; }
            if (peek() == '}') { i++; break; }
            throw std::runtime_error("expected ',' or '}' at pos " + std::to_string(i));
        }
        return j;
    }
    Json parseArray() {
        Json j = Json::Arr();
        expect('['); skipWs();
        if (peek() == ']') { i++; return j; }
        while (true) {
            Json val = parseValue();
            j.arr.push_back(std::move(val));
            skipWs();
            if (peek() == ',') { i++; continue; }
            if (peek() == ']') { i++; break; }
            throw std::runtime_error("expected ',' or ']' at pos " + std::to_string(i));
        }
        return j;
    }
    std::string parseString() {
        skipWs(); expect('"');
        std::string out; out.reserve(1024);
        while (true) {
            if (i >= s.size()) throw std::runtime_error("unterminated string at pos " + std::to_string(i));
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\') {
                if (i >= s.size()) throw std::runtime_error("bad escape at pos " + std::to_string(i));
                char e = s[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i + 4 > s.size()) throw std::runtime_error("bad unicode escape");
                        std::string hex = s.substr(i, 4); i += 4;
                        unsigned int cp = (unsigned int)std::stoul(hex, nullptr, 16);
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        // Not a standard JSON escape (JSON only defines the ones above).
                        // Regex patterns routinely contain \w \s \d \. \( \) etc, and an
                        // LLM emitting {"q":["\\w+"]} means "the two chars \ and w in the
                        // pattern", not "drop the backslash". Keep both chars so those
                        // escapes reach std::regex intact instead of silently corrupting
                        // into a literal 'w'/'s'/'d'.
                        out += '\\';
                        out += e;
                        break;
                }
            } else out += c;
        }
        return out;
    }
    Json parseBool() {
        if (s.compare(i, 4, "true") == 0)  { i += 4; return Json::Bool(true); }
        if (s.compare(i, 5, "false") == 0) { i += 5; return Json::Bool(false); }
        throw std::runtime_error("invalid literal at pos " + std::to_string(i));
    }
    Json parseNull() {
        if (s.compare(i, 4, "null") == 0) { i += 4; return Json::Null_(); }
        throw std::runtime_error("invalid literal at pos " + std::to_string(i));
    }
    Json parseNumber() {
        size_t start = i;
        if (peek() == '-') i++;
        while (isdigit((unsigned char)peek())) i++;
        if (peek() == '.') { i++; while (isdigit((unsigned char)peek())) i++; }
        if (peek() == 'e' || peek() == 'E') {
            i++;
            if (peek() == '+' || peek() == '-') i++;
            while (isdigit((unsigned char)peek())) i++;
        }
        if (i == start) throw std::runtime_error("invalid number at pos " + std::to_string(i));
        return Json::Num(std::stod(s.substr(start, i - start)));
    }
};

Json Json::parse(const std::string& text, std::string* err) {
    try {
        JsonParser p(text);
        Json v = p.parseValue();
        p.skipWs();
        if (p.i != text.size()) {
            throw std::runtime_error("unexpected trailing data at pos " + std::to_string(p.i));
        }
        return v;
    } catch (std::exception& e) {
        if (err) *err = e.what();
        return Json::Null_();
    }
}

static void jsonEscape(const std::string& s, std::string& out) {
    out.reserve(s.size() + 32);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else out += c;
        }
    }
}

std::string Json::dump() const {
    std::ostringstream out;
    switch (type) {
        case Type::Null:   out << "null"; break;
        case Type::Bool:   out << (b ? "true" : "false"); break;
        case Type::Number:
            if (num == (double)(long long)num) out << (long long)num;
            else out << num;
            break;
        case Type::String: {
            std::string esc; jsonEscape(str, esc);
            out << '"' << esc << '"'; break;
        }
        case Type::Array: {
            out << "[";
            for (size_t k = 0; k < arr.size(); ++k) {
                if (k) out << ",";
                out << arr[k].dump();
            }
            out << "]"; break;
        }
        case Type::Object: {
            out << "{";
            for (size_t k = 0; k < obj.size(); ++k) {
                if (k) out << ",";
                std::string esc; jsonEscape(obj[k].first, esc);
                out << "\"" << esc << "\":" << obj[k].second.dump();
            }
            out << "}"; break;
        }
    }
    return out.str();
}

// ===========================================================================
// Generic utilities
// ===========================================================================
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string toLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}
std::string truncateOutput(const std::string& s, size_t maxLen = 6000) {
    if (s.size() <= maxLen) return s;
    size_t headLen = 800;
    size_t tailLen = maxLen - headLen - 80;
    std::string head = s.substr(0, headLen);
    std::string tail = s.substr(s.size() - tailLen);
    return head + "\n...[" + std::to_string(s.size() - headLen - tailLen) + " chars omitted]...\n" + tail;
}
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    out += "'";
    return out;
}
std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : content) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c == '\r') continue;
        else cur += c;
    }
    out.push_back(cur);
    if (!out.empty() && out.back().empty() && !content.empty() && content.back() == '\n') out.pop_back();
    return out;
}
// Same line-count semantics as splitLines(normalizeLineEndings(x)).size(), but
// O(1) extra memory instead of allocating a std::string per line + a full
// normalized copy of content. For a large create_file payload, the old path
// was the actual dominant source of peak RSS (millions of small heap allocs).
size_t countLines(const std::string& content) {
    if (content.empty()) return 1; // matches splitLines("") == 1
    size_t breaks = 0;
    bool endsWithBreak = false;
    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];
        if (c == '\r') {
            breaks++; endsWithBreak = true;
            if (i + 1 < content.size() && content[i + 1] == '\n') i++;
        } else if (c == '\n') {
            breaks++; endsWithBreak = true;
        } else {
            endsWithBreak = false;
        }
    }
    return endsWithBreak ? breaks : breaks + 1;
}
std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (auto& l : lines) { out += l; out += '\n'; }
    return out;
}
std::string readFileAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss; ss << in.rdbuf();
    return ss.str();
}
std::vector<std::string> readFileLines(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> lines;
    if (!in) return lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}
bool writeFileAll(const std::string& path, const std::string& content) {
    // Atomic replace on POSIX: write a sibling temporary file, fsync it, then rename.
    // Do not fall back to truncating the target: that defeats the crash-safety guarantee.
    fs::path target(path);
    fs::path tmp = target;
    tmp += ".tmp." + std::to_string((long long) ::getpid()) + "." +
           std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), (std::streamsize)content.size());
        if (!out) return false;
        out.flush();
        if (!out) return false;
    }
    int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd < 0) {
        std::error_code ec; fs::remove(tmp, ec);
        return false;
    }
    bool ok = (::fsync(fd) == 0);
    ::close(fd);
    if (!ok) {
        std::error_code ec; fs::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    // Best effort directory durability. The rename itself is already atomic.
    fs::path parent = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    return true;
}
long lineNumberAt(const std::string& content, size_t pos) {
    long n = 1;
    for (size_t k = 0; k < pos && k < content.size(); ++k) if (content[k] == '\n') n++;
    return n;
}
// Forward-declared here (defined below) so backupSafeName can hash the full
// relative path to avoid backup-filename collisions.
std::string fnv1a64Hex(const std::string& s);

// Turns a relative path into a name safe for the flat .mpt_backups directory.
// Prefixed with a hash of the FULL path so distinct paths that flatten to the
// same string after '/'->'_' substitution (e.g. "src/foo.cpp" vs "src_foo.cpp")
// never collide and mix undo histories.
std::string backupSafeName(const std::string& relFile) {
    std::string flat = relFile;
    for (auto& c : flat) if (c == '/' || c == '\\') c = '_';
    return fnv1a64Hex(relFile).substr(0, 8) + "_" + flat;
}

void backupFile(const fs::path& baseDir, const std::string& relFile, const std::string& originalContent) {
    try {
        // Backups live in the OWNING root's .mpt_backups, not whichever root
        // the calling tool was handed — cross-root edits keep their history
        // beside the file they belong to.
        fs::path bdir = rootFor(baseDir, relFile) / ".mpt_backups";
        std::error_code ec;
        fs::create_directories(bdir, ec);
        std::string safeName = backupSafeName(relFile);
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        fs::path bfile = bdir / (safeName + "." + std::to_string(now) + ".bak");
        std::ofstream out(bfile, std::ios::binary);
        if (out) out << originalContent;
    } catch (...) {}
}
// Lenient base64 decoder: skips whitespace/newlines/invalid chars, stops at '='.
// Lets callers send old/new/content text as base64 to sidestep JSON-escaping
// pain entirely (no backslash-doubling for regex-y source text).
std::string base64Decode(const std::string& in) {
    static int T[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[(unsigned char)chars[i]] = i;
        init = true;
    }
    std::string out; out.reserve(in.size() * 3 / 4 + 3);
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = T[c];
        if (d == -1) continue; // skip whitespace/newlines/anything not base64
        val = (val << 6) + d;
        bits += 6;
        if (bits >= 0) {
            out.push_back((char)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
// Reads a text field that may be supplied either as a plain JSON string
// (shortKey/longKey) or as base64 (b64Key). base64 wins if both are present
// and non-empty, since it's the unambiguous form.
std::string getTextOrB64(const Json& j, const std::string& shortKey,
                         const std::string& longKey, const std::string& b64Key) {
    const Json* b64p = j.get(b64Key);
    if (b64p && b64p->type == Json::Type::String && !b64p->str.empty()) {
        return base64Decode(b64p->str);
    }
    return j.getStr2(shortKey, longKey);
}
// Cheap, fast, non-cryptographic content checksum (FNV-1a 64-bit) so callers
// can confirm a write landed as expected without a follow-up read.
std::string fnv1a64Hex(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}
std::string normalizeLineEndings(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (size_t i=0;i<s.size();++i) {
        if (s[i]=='\r') {
            if (i+1 < s.size() && s[i+1]=='\n') { out.push_back('\n'); ++i; }
            else out.push_back('\n');
        } else out.push_back(s[i]);
    }
    return out;
}
// Optional content-drift guard for line-number-addressed edits (patch's
// replace_lines/delete_lines, edit's delete/insert_after/insert_before). The
// caller may pass "exp" (exact text expected to currently occupy the target
// line[s]) and/or "expH" (fnv1a64Hex of that text). If either is supplied and
// doesn't match what's actually there, the mutation is refused instead of
// silently rewriting whatever currently sits at that line number -- closing
// the gap between the system prompt's "read fresh before patching" advice and
// what the tool itself verifies.
bool verifyExpectedContent(const Json& p, const std::vector<std::string>& lines,
                           size_t adjStart, size_t adjEnd, std::string& failReason) {
    std::string expect = p.getStr2("exp", "expect");
    std::string expectHash = p.getStr2("expH", "expect_hash");
    if (expect.empty() && expectHash.empty()) return true; // no assertion requested
    std::ostringstream actual;
    for (size_t i = adjStart; i <= adjEnd && i < lines.size(); ++i) {
        actual << lines[i];
        if (i != adjEnd) actual << "\n";
    }
    std::string actualText = actual.str();
    if (!expect.empty() && normalizeLineEndings(expect) != normalizeLineEndings(actualText)) {
        failReason = "'exp' doesn't match what's currently at those line(s); file likely changed "
                      "since it was last read. Re-read the file and retry.";
        return false;
    }
    if (!expectHash.empty() && fnv1a64Hex(actualText) != expectHash) {
        failReason = "'expH' doesn't match the hash of what's currently at those line(s); file "
                      "likely changed since it was last read. Re-read the file and retry.";
        return false;
    }
    return true;
}
// Returns every start index at which oldLines matches fileLines (optionally
// whitespace-trimmed), instead of just the first, so fuzzy (whitespace-
// insensitive) matches can be checked for ambiguity the same way exact and
// normalized-newline matches already are.
std::vector<size_t> findAllLinesSequences(const std::vector<std::string>& fileLines,
                                          const std::vector<std::string>& oldLines,
                                          bool trimWhitespace) {
    std::vector<size_t> out;
    if (oldLines.empty() || oldLines.size() > fileLines.size()) return out;
    for (size_t i=0;i+oldLines.size()<=fileLines.size();++i) {
        bool ok=true;
        for (size_t j=0;j<oldLines.size();++j) {
            std::string a = fileLines[i+j];
            std::string b = oldLines[j];
            if (trimWhitespace) { a = trim(a); b = trim(b); }
            if (a != b) { ok=false; break; }
        }
        if (ok) { out.push_back(i); if (out.size() > 10000) break; }
    }
    return out;
}

// Resolves relPath against baseDir and refuses to leave the sandbox.
// Two escape routes this closes:
//   1. fs::path's operator/ silently DISCARDS baseDir when relPath is
//      absolute (baseDir / "/etc/passwd" == "/etc/passwd", not an error).
//   2. "../../whatever" can walk a relative path above baseDir even though
//      each individual segment looks harmless.
// Returns false (and leaves out untouched) if relPath would resolve outside
// baseDir. Uses weakly_canonical so this works for paths that don't exist
// yet (e.g. a file about to be created).
static bool safeResolveIn(const fs::path& root, const std::string& relPath, fs::path& out) {
    fs::path rp(relPath);
    std::error_code ec;
    fs::path normBase = fs::weakly_canonical(root, ec);
    if (ec) normBase = root.lexically_normal();
    fs::path combined = rp.is_absolute() ? rp : (root / rp); // absolute rp replaces root
    fs::path normCombined = fs::weakly_canonical(combined, ec);
    if (ec) normCombined = combined.lexically_normal();
    auto bi = normBase.begin(), be = normBase.end();
    auto ci = normCombined.begin(), ce = normCombined.end();
    for (; bi != be; ++bi, ++ci) {
        if (ci == ce || *ci != *bi) return false; // combined isn't under root
    }
    out = combined;
    return true;
}

bool safeResolve(const fs::path& baseDir, const std::string& relPath, fs::path& out) {
    if (relPath.empty()) return false;
    // Multi-root resolution (sandbox = union of registered roots):
    //   1. A relative path that does not exist under baseDir but does exist
    //      under a sibling root resolves to the sibling — the real file wins
    //      over a same-named primary path that isn't there.
    //   2. Otherwise baseDir gets first claim: single-root behavior is
    //      unchanged, and brand-new relative paths are created in the
    //      primary workspace.
    //   3. Finally, absolute paths and ../ relatives are accepted if they
    //      land inside any other registered root.
    std::error_code ec;
    fs::path rp(relPath);
    if (!rp.is_absolute()) {
        bool underBase = fs::exists(baseDir / rp, ec);
        if (!underBase) {
            for (auto& r : g_roots) {
                if (r == baseDir) continue;
                if (fs::exists(r / rp, ec) && safeResolveIn(r, relPath, out)) return true;
            }
        }
    }
    if (safeResolveIn(baseDir, relPath, out)) return true;
    for (auto& r : g_roots) {
        if (r == baseDir) continue;
        if (safeResolveIn(r, relPath, out)) return true;
    }
    return false;
}
// Returns true if `full` (already safeResolve'd) IS baseDir itself, or is/lives
// inside baseDir/.mpt_backups. Destructive ops (delete, and move/rename onto
// this path) must refuse these: deleting baseDir wipes the whole project in one
// call, and deleting/overwriting .mpt_backups silently disables undo.
bool isProtectedPath(const fs::path& baseDir, const fs::path& full) {
    std::error_code ec;
    fs::path normBase = fs::weakly_canonical(baseDir, ec);
    if (ec) normBase = baseDir.lexically_normal();
    fs::path normFull = fs::weakly_canonical(full, ec);
    if (ec) normFull = full.lexically_normal();
    if (normFull == normBase) return true;
    fs::path normBackups = fs::weakly_canonical(baseDir / ".mpt_backups", ec);
    if (ec) normBackups = (baseDir / ".mpt_backups").lexically_normal();
    auto bi = normBackups.begin(), be = normBackups.end();
    auto fi = normFull.begin(), fe = normFull.end();
    for (; bi != be; ++bi, ++fi) {
        if (fi == fe || *fi != *bi) return false;
    }
    return true; // normFull == normBackups or nested inside it
}
bool isBinaryContent(const std::string& content) {
    size_t checkLen = std::min<size_t>(8192, content.size());
    if (checkLen == 0) return false;
    size_t nonText = 0;
    size_t i = 0;
    while (i < checkLen) {
        unsigned char c = (unsigned char)content[i];
        if (c == 0) return true; // NUL byte: near-universal binary signal, short-circuit
        if (c == '\n' || c == '\r' || c == '\t') { i++; continue; }
        if (c < 0x20 || c == 0x7F) { nonText++; i++; continue; } // control chars
        if (c < 0x80) { i++; continue; } // printable ASCII
        // c >= 0x80: only count as binary if it's NOT a valid UTF-8 multi-byte
        // sequence, so accented text / CJK / box-drawing / emoji aren't flagged.
        int seqLen = 0;
        if ((c & 0xE0) == 0xC0) seqLen = 2;
        else if ((c & 0xF0) == 0xE0) seqLen = 3;
        else if ((c & 0xF8) == 0xF0) seqLen = 4;
        else { nonText++; i++; continue; } // invalid lead byte / stray continuation byte
        if (i + (size_t)seqLen > checkLen) { nonText++; i++; continue; } // truncated sequence
        bool valid = true;
        for (int k = 1; k < seqLen; ++k) {
            if (((unsigned char)content[i + k] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid) { nonText++; i++; continue; }
        i += (size_t)seqLen; // valid multi-byte UTF-8 char: counts as text
    }
    return nonText > checkLen / 8;
}

// Content-based binary check for one file: reads only the first 8192 bytes.
// Extension checks can never catch extensionless artifacts (compiled
// binaries, suffix-less images) — this is the ground truth the walk filter
// and the bin tags trust.
bool looksBinaryFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::string head(8192, '\0');
    in.read(&head[0], (std::streamsize)head.size());
    head.resize((size_t)in.gcount());
    return isBinaryContent(head);
}

// ===========================================================================
// Timeout-aware shell execution (fork + execvp + select with deadline)
// ===========================================================================
std::pair<int, std::string> runShellTimed(const std::string& cmd, const fs::path& cwd, long timeoutSec) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {-1001, "pipe() failed"};

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return {-1001, "fork() failed"};
    }
    if (pid == 0) {
        // Put the shell and all descendants in a dedicated process group so timeout
        // and output-cap termination cannot leave npm/python/etc. children running.
        setpgid(0, 0);
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(pipefd[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        std::string full = "cd " + shellQuote(cwd.string()) + " && (" + cmd + ") 2>&1";
        execl("/bin/sh", "sh", "-c", full.c_str(), (char*)nullptr);
        _exit(127);
    }
    // Race-safe on normal POSIX systems: child also calls setpgid, so whichever wins
    // first establishes the same process-group ID.
    setpgid(pid, pid);
    close(pipefd[1]);

    std::string output;
    constexpr size_t OUTPUT_CAP = 8 * 1024 * 1024;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    bool timedOut = false;
    bool outputLimited = false;
    std::array<char, 8192> buf{};

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    while (true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining.count() <= 0) { timedOut = true; break; }
        timeval tv{};
        tv.tv_sec = remaining.count() / 1000;
        tv.tv_usec = (remaining.count() % 1000) * 1000;
        int sel = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) { timedOut = true; break; }
        while (true) {
            ssize_t n = read(pipefd[0], buf.data(), buf.size());
            if (n > 0) {
                if (output.size() + (size_t)n > OUTPUT_CAP) {
                    size_t keep = OUTPUT_CAP - output.size();
                    output.append(buf.data(), keep);
                    outputLimited = true;
                    break;
                }
                output.append(buf.data(), (size_t)n);
                continue;
            }
            if (n == 0) { close(pipefd[0]); int status; waitpid(pid, &status, 0);
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
                return {code, output}; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close(pipefd[0]); int status; waitpid(pid, &status, 0);
            return {-1001, output + "\n[nexon] read() failed: " + std::strerror(errno)};
        }
        if (outputLimited) break;
    }

    close(pipefd[0]);
    if (timedOut || outputLimited) {
        // Kill the entire process group, not just the shell.
        kill(-pid, SIGTERM);
        auto killDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
        int status = 0;
        while (std::chrono::steady_clock::now() < killDeadline) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) break;
            if (w < 0 && errno != EINTR) break;
            usleep(20000);
        }
        if (waitpid(pid, &status, WNOHANG) == 0) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        if (timedOut) return {-1000, output + "\n[nexon] TIMEOUT after " + std::to_string(timeoutSec) + "s"};
        return {-1002, output + "\n[nexon] OUTPUT_LIMIT: command exceeded 8MB"};
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    return {code, output};
}

// ===========================================================================
// Unified diff parser
// ===========================================================================
struct DiffHunk {
    long oldStart, oldCount, newStart, newCount;
    std::vector<std::string> lines;
};
std::vector<DiffHunk> parseUnifiedDiff(const std::string& diffText) {
    std::vector<DiffHunk> hunks;
    auto lines = splitLines(diffText);
    size_t i = 0;
    while (i < lines.size()) {
        if (lines[i].substr(0, 2) == "@@") break;
        i++;
    }
    while (i < lines.size()) {
        if (lines[i].substr(0, 2) != "@@") { i++; continue; }
        std::string header = lines[i];
        long os=0, oc=1, ns=0, nc=1;
        size_t minusPos = header.find('-');
        size_t plusPos  = header.find('+');
        if (minusPos == std::string::npos || plusPos == std::string::npos) { i++; continue; }
        {
            std::string oldRange = trim(header.substr(minusPos + 1, plusPos - minusPos - 1));
            size_t comma = oldRange.find(',');
            if (comma == std::string::npos) { os = std::stol(oldRange); oc = 1; }
            else { os = std::stol(oldRange.substr(0, comma)); oc = std::stol(oldRange.substr(comma + 1)); }
        }
        {
            std::string rest = header.substr(plusPos + 1);
            size_t endPos = rest.find_first_of(" @");
            std::string newRange = trim((endPos == std::string::npos) ? rest : rest.substr(0, endPos));
            size_t comma = newRange.find(',');
            if (comma == std::string::npos) { ns = std::stol(newRange); nc = 1; }
            else { ns = std::stol(newRange.substr(0, comma)); nc = std::stol(newRange.substr(comma + 1)); }
        }
        DiffHunk h{os, oc, ns, nc, {}};
        i++;
        while (i < lines.size() && lines[i].substr(0, 2) != "@@") {
            if (lines[i].substr(0, 3) == "---" || lines[i].substr(0, 3) == "+++") break;
            h.lines.push_back(lines[i]);
            i++;
        }
        hunks.push_back(h);
    }
    return hunks;
}
// Map an original (pre-batch) line number to its current line index. origins[i] is
// the original line number still represented by current line i; inserted/generated
// lines carry 0. This prevents the classic "global line offset" bug where an edit at
// line 1 shifts an unrelated edit intended for line 50.
static bool currentIndexForOriginalLine(const std::vector<long>& origins, long originalLine, size_t& outIndex) {
    if (originalLine < 1) return false;
    for (size_t i = 0; i < origins.size(); ++i) {
        if (origins[i] == originalLine) { outIndex = i; return true; }
    }
    return false;
}

static long currentLineNumberForOriginalLine(const std::vector<long>& origins, long originalLine) {
    size_t idx = 0;
    return currentIndexForOriginalLine(origins, originalLine, idx) ? (long)idx + 1 : 0;
}

static void initOriginalOrigins(const std::vector<std::string>& lines, std::vector<long>& origins) {
    origins.resize(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) origins[i] = (long)i + 1;
}

static void replaceLineRange(std::vector<std::string>& lines, std::vector<long>& origins,
                             size_t start, size_t oldCount, const std::vector<std::string>& newLines) {
    std::vector<long> replacementOrigins;
    replacementOrigins.reserve(newLines.size());
    for (size_t i = 0; i < newLines.size(); ++i) {
        // Preserve original identity for as many replacement slots as possible.
        // Extra generated lines have no original-line identity (0).
        replacementOrigins.push_back(i < oldCount && start + i < origins.size() ? origins[start + i] : 0);
    }
    lines.erase(lines.begin() + (long)start, lines.begin() + (long)(start + oldCount));
    origins.erase(origins.begin() + (long)start, origins.begin() + (long)(start + oldCount));
    lines.insert(lines.begin() + (long)start, newLines.begin(), newLines.end());
    origins.insert(origins.begin() + (long)start, replacementOrigins.begin(), replacementOrigins.end());
}

struct MappedDiffResult { bool ok; std::string error; std::vector<std::string> lines; std::vector<long> origins; long delta = 0; };

static bool tryApplyMappedHunk(const std::vector<std::string>& working,
                               const DiffHunk& h, long startPos,
                               std::vector<std::string>& newContent,
                               std::vector<long>* consumedOrigins) {
    std::vector<std::string> verifiedOld;
    newContent.clear();
    if (startPos < 0 || startPos > (long)working.size()) return false;
    long lineIdx = startPos;
    for (const auto& hl : h.lines) {
        char tag = hl.empty() ? ' ' : hl[0];
        std::string content = (hl.size() > 1) ? hl.substr(1) : "";
        if (tag == ' ') {
            if (lineIdx >= (long)working.size()) return false;
            if (working[lineIdx] != content && trim(working[lineIdx]) != trim(content)) return false;
            verifiedOld.push_back(working[lineIdx]);
            newContent.push_back(working[lineIdx]); // preserve actual context text exactly
            lineIdx++;
        } else if (tag == '-') {
            if (lineIdx >= (long)working.size()) return false;
            if (working[lineIdx] != content && trim(working[lineIdx]) != trim(content)) return false;
            verifiedOld.push_back(working[lineIdx]);
            lineIdx++;
        } else if (tag == '+') {
            newContent.push_back(content);
        } else if (tag == '\\') {
            continue;
        } else {
            return false;
        }
    }
    if ((long)verifiedOld.size() != h.oldCount || (long)newContent.size() != h.newCount) return false;
    if (consumedOrigins) consumedOrigins->assign((size_t)verifiedOld.size(), 0);
    return true;
}

static MappedDiffResult applyHunksMapped(const std::vector<std::string>& fileLines,
                                         const std::vector<long>& origins,
                                         const std::vector<DiffHunk>& hunks) {
    MappedDiffResult result{false, "", fileLines, origins, 0};
    for (size_t hi = 0; hi < hunks.size(); ++hi) {
        const DiffHunk& h = hunks[hi];
        size_t expected = 0;
        bool hasMappedStart = (h.oldStart == 0) ? true : currentIndexForOriginalLine(result.origins, h.oldStart, expected);
        if (!hasMappedStart) {
            result.error = "hunk " + std::to_string(hi + 1) + " targets original line " +
                           std::to_string(h.oldStart) + " which was deleted/replaced earlier in this batch; refusing to guess";
            return result;
        }
        long actual = (long)expected;
        std::vector<std::string> newContent;
        bool matched = tryApplyMappedHunk(result.lines, h, actual, newContent, nullptr);
        // Small location drift is allowed when unrelated insertions are present, but only
        // while still requiring the exact hunk context; no prefix/partial matching. If more
        // than one position in the drift window matches, that's ambiguous (e.g. repetitive
        // boilerplate) and we refuse to silently pick one rather than take the first hit.
        if (!matched) {
            std::vector<long> candidates;
            for (long delta = 1; delta <= 20; ++delta) {
                std::vector<std::string> tmp;
                if (actual + delta <= (long)result.lines.size() &&
                    tryApplyMappedHunk(result.lines, h, actual + delta, tmp, nullptr)) {
                    candidates.push_back(actual + delta);
                }
                if (actual - delta >= 0 &&
                    tryApplyMappedHunk(result.lines, h, actual - delta, tmp, nullptr)) {
                    candidates.push_back(actual - delta);
                }
            }
            if (candidates.size() == 1) {
                matched = tryApplyMappedHunk(result.lines, h, candidates[0], newContent, nullptr);
                actual = candidates[0];
            } else if (candidates.size() > 1) {
                result.error = "hunk " + std::to_string(hi + 1) + " ambiguous: " +
                               std::to_string(candidates.size()) +
                               " equally-good matches within +-20 lines of original line " +
                               std::to_string(h.oldStart) + "; refusing to guess which one";
                return result;
            }
        }
        if (!matched) {
            result.error = "hunk " + std::to_string(hi + 1) + " context mismatch at original line " +
                           std::to_string(h.oldStart);
            return result;
        }
        size_t oldCount = (size_t)h.oldCount;
        size_t newCount = (size_t)h.newCount;
        std::vector<long> inherited;
        inherited.reserve(newCount);
        for (size_t j = 0; j < oldCount; ++j) inherited.push_back(result.origins[(size_t)actual + j]);
        std::vector<long> newOrigins(newCount, 0);
        for (size_t j = 0; j < newCount && j < inherited.size(); ++j) newOrigins[j] = inherited[j];
        result.lines.erase(result.lines.begin() + actual, result.lines.begin() + actual + oldCount);
        result.origins.erase(result.origins.begin() + actual, result.origins.begin() + actual + oldCount);
        result.lines.insert(result.lines.begin() + actual, newContent.begin(), newContent.end());
        result.origins.insert(result.origins.begin() + actual, newOrigins.begin(), newOrigins.end());
        result.delta += h.newCount - h.oldCount;
    }
    result.ok = true;
    return result;
}

// ===========================================================================
// File collection
// ===========================================================================
static const std::vector<std::string> IGNORE_DIRS = {
    ".git", "node_modules", "build", "dist", "target", ".venv", "venv",
    "__pycache__", ".idea", ".vscode", ".gradle", ".cache", ".mpt_backups"
};
static const std::vector<std::string> BINARY_EXT = {
    ".png",".jpg",".jpeg",".gif",".bmp",".ico",".pdf",".zip",".tar",".gz",
    ".7z",".exe",".o",".obj",".so",".dll",".dylib",".class",".jar",".bin",
    ".woff",".woff2",".ttf",".eot",".mp3",".mp4",".mov",".avi",".webm",
    ".pyc",".apk",".aar"
};
bool isIgnoredDir(const std::string& name) {
    for (auto& d : IGNORE_DIRS) if (name == d) return true;
    return false;
}
bool isBinaryExt(const fs::path& p) {
    std::string ext = toLower(p.extension().string());
    for (auto& b : BINARY_EXT) if (ext == b) return true;
    return false;
}
void walkDir(const fs::path& dir, std::vector<fs::path>& out, bool includeBinaries = false) {
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    if (ec) return;
    for (; it != end; ) {
        const auto& entry = *it;
        std::error_code dec;
        if (entry.is_directory(dec)) {
            if (!dec && isIgnoredDir(entry.path().filename().string())) {
                it.disable_recursion_pending();
            }
            it.increment(ec);
            if (ec) break;
            continue;
        }
        std::error_code rec;
        if (entry.is_regular_file(rec) && !rec) {
            if (includeBinaries) {
                out.push_back(entry.path()); // listing mode: no binary/size filter — 'recent' must not hide a just-copied image
            } else if (!isBinaryExt(entry.path()) && !looksBinaryFile(entry.path())) {
                // content sniff catches extensionless binaries (compiled
                // artifacts) that the extension list can never match
                std::error_code sec;
                auto sz = fs::file_size(entry.path(), sec);
                if (sec || sz <= 3 * 1024 * 1024) out.push_back(entry.path());
            }
        }
        it.increment(ec);
        if (ec) break;
    }
}
std::vector<fs::path> collectFiles(const fs::path& baseDir, const std::vector<std::string>& subpaths, std::vector<std::string>* missing = nullptr) {
    std::vector<fs::path> out;
    if (subpaths.empty()) { walkDir(baseDir, out); return out; }
    for (auto& sp : subpaths) {
        fs::path full;
        if (!safeResolve(baseDir, sp, full)) {
            if (missing) missing->push_back(sp + " (outside project sandbox)");
            continue;
        }
        std::error_code ec;
        if (!fs::exists(full, ec)) {
            if (missing) missing->push_back(sp);
            continue;
        }
        if (fs::is_directory(full, ec)) walkDir(full, out);
        else if (fs::is_regular_file(full, ec)) out.push_back(full);
    }
    return out;
}

// ===========================================================================
// Tool: shell_command_tool  (alias: sh)  — with timeout
// ===========================================================================
Json toolShellCommand(const Json& args, const fs::path& baseDir) {
    std::string cmd = args.getStr2("cmd", "command");
    if (cmd.empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("'cmd' is required"));
        return r;
    }
    long maxLen = args.getInt2("max", "max_output", 6000);
    long timeout = args.getInt2("to", "timeout", 30);
    if (timeout < 1) timeout = 30;
    if (timeout > 600) timeout = 600;
    fs::path cwd = baseDir;
    std::string dirArg = args.getStr2("dir", "cwd", "");
    if (!dirArg.empty()) {
        fs::path resolved;
        if (!safeResolve(baseDir, dirArg, resolved)) {
            Json r = Json::Obj();
            r.set("err", Json::Str("'dir' is outside all registered workspaces: " + dirArg));
            return r;
        }
        std::error_code dec;
        fs::path canon = fs::canonical(resolved, dec);
        if (dec || !fs::is_directory(canon, dec) || !isRegisteredRoot(baseDir, canon)) {
            Json r = Json::Obj();
            r.set("err", Json::Str("'dir' must be a registered workspace root, got: " + dirArg));
            return r;
        }
        cwd = canon;
    }
    auto [code, output] = runShellTimed(cmd, cwd, timeout);
    Json r = Json::Obj();
    r.set("rc", Json::Num(code));
    if (code == -1000) r.set("err", Json::Str("timeout"));
    else if (code == -1001) r.set("err", Json::Str("launch_failed"));
    r.set("out", Json::Str(truncateOutput(output, maxLen)));
    return r;
}

// ===========================================================================
// Tool: multi_search_tool  (alias: search)
// ===========================================================================
Json toolMultiSearch(const Json& args, const fs::path& baseDir) {
    const std::vector<Json>* queries = args.getArr2("q", "queries");
    if (!queries || queries->empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("'q' array is required"));
        return r;
    }
    const std::vector<Json>* paths = args.getArr2("paths", "paths");
    std::vector<std::string> subpaths;
    if (paths) for (auto& p : *paths) if (p.type == Json::Type::String) subpaths.push_back(p.str);
    bool caseSensitive = args.getBool2("cs", "case_sensitive", true);
    bool useRegex = args.getBool2("re", "regex", false);
    long maxPerQuery = args.getInt2("max", "max_results_per_query", 100);
    if (maxPerQuery < 1) maxPerQuery = 100;
    if (maxPerQuery > 10000) maxPerQuery = 10000;
    long context = args.getInt2("ctx", "context", 0);
    if (context < 0) context = 0;
    if (context > 20) context = 20;

    std::vector<std::string> missing;
    std::vector<fs::path> files = collectFiles(baseDir, subpaths, &missing);
    Json outResults = Json::Arr();

    for (auto& q : *queries) {
        std::string query = (q.type == Json::Type::String) ? q.str : "";
        if (query.empty()) continue;
        Json qres = Json::Obj();
        long count = 0;
        bool truncated = false;
        std::regex re;
        bool reOk = true;
        if (useRegex) {
            try {
                re = std::regex(query, caseSensitive ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
            } catch (const std::exception& e) {
                reOk = false;
                qres.set("err", Json::Str(std::string("regex error: ") + e.what()));
                outResults.arr.push_back(qres);
                continue;
            }
        }
        std::ostringstream matchText;
        std::string currentFile;
        bool inFile = false;
        for (auto& fp : files) {
            if (count >= maxPerQuery) { truncated = true; break; }
            std::error_code ec;
            std::string relName = fs::relative(fp, baseDir, ec).string();
            if (ec) relName = fp.string();
            std::vector<std::string> lines = readFileLines(fp.string());
            bool fileHadMatch = false;
            for (size_t i = 0; i < lines.size(); ++i) {
                if (count >= maxPerQuery) { truncated = true; break; }
                bool matched = false;
                try {
                    if (useRegex && reOk) matched = std::regex_search(lines[i], re);
                    else if (caseSensitive) matched = lines[i].find(query) != std::string::npos;
                    else matched = toLower(lines[i]).find(toLower(query)) != std::string::npos;
                } catch (...) { matched = false; }
                if (matched) {
                    if (count >= maxPerQuery) { truncated = true; break; }
                    if (!fileHadMatch) {
                        if (inFile) matchText << "--\n";
                        matchText << "f:" << relName << "\n";
                        inFile = true;
                        fileHadMatch = true;
                    }
                    if (context > 0) {
                        long startI = std::max<long>(0, (long)i - context);
                        long endI = std::min<long>((long)lines.size() - 1, (long)i + context);
                        for (long k = startI; k <= endI; ++k) {
                            char sep = (k == (long)i) ? ':' : '~';
                            matchText << "  " << (k + 1) << sep << lines[k] << "\n";
                        }
                    } else {
                        matchText << "  " << (i + 1) << ":" << lines[i] << "\n";
                    }
                    count++;
                }
            }
            if (truncated) break;
        }
        if (inFile) matchText << "--\n";
        qres.set("n", Json::Num((double)count));
        qres.set("trunc", Json::Bool(truncated));
        qres.set("m", Json::Str(matchText.str()));
        outResults.arr.push_back(qres);
    }
    Json out = Json::Obj();
    out.set("files", Json::Num((double)files.size()));
    if (!missing.empty()) {
        Json marr = Json::Arr();
        for (auto& m : missing) marr.arr.push_back(Json::Str(m));
        out.set("missing_paths", marr);
    }
    out.set("r", outResults);
    return out;
}

// ===========================================================================
// Tool: multi_read_tool  (alias: read)  — with binary detection
// ===========================================================================
Json toolMultiRead(const Json& args, const fs::path& baseDir) {
    const std::vector<Json>* reads = args.getArr2("r", "reads");
    if (!reads || reads->empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("'r' array is required"));
        return r;
    }
    const long MAX_LINES = 4000;
    Json filesOut = Json::Arr();
    for (auto& rd : *reads) {
        std::string relFile = rd.getStr2("f", "file");
        if (relFile.empty()) {
            Json entry = Json::Obj();
            entry.set("err", Json::Str("'f' (file) required"));
            filesOut.arr.push_back(entry);
            continue;
        }
        fs::path full;
        Json entry = Json::Obj();
        entry.set("f", Json::Str(relFile));
        if (!safeResolve(baseDir, relFile, full)) {
            entry.set("err", Json::Str("path escapes project sandbox"));
            filesOut.arr.push_back(entry);
            continue;
        }
        std::error_code ec;
        if (!fs::exists(full, ec)) {
            entry.set("err", Json::Str("not found"));
            filesOut.arr.push_back(entry);
            continue;
        }
        std::string raw = readFileAll(full.string());
        if (isBinaryContent(raw)) {
            entry.set("err", Json::Str("binary file (use shell tool if needed)"));
            entry.set("size", Json::Num((double)raw.size()));
            filesOut.arr.push_back(entry);
            continue;
        }
        std::vector<std::string> lines = readFileLines(full.string());
        long start = rd.getInt2("s", "start", 1);
        long end = rd.getInt2("e", "end", 0);
        if (start < 1) start = 1;
        if (end <= 0 || end > (long)lines.size()) end = (long)lines.size();
        if (start > (long)lines.size()) {
            entry.set("err", Json::Str("start > file lines (" + std::to_string(lines.size()) + ")"));
            filesOut.arr.push_back(entry);
            continue;
        }
        if (end - start + 1 > MAX_LINES) {
            end = start + MAX_LINES - 1;
            entry.set("trunc", Json::Bool(true));
        }
        bool lineNumbers = rd.getBool2("ln", "line_numbers", true);
        long lastLine = std::min<long>(end, (long)lines.size());
        std::ostringstream out;
        if (lineNumbers) {
            int width = (int)std::to_string(lastLine).size();
            for (long i = start; i <= end && i <= (long)lines.size(); ++i) {
                char buf[32]; snprintf(buf, sizeof(buf), "%*ld: ", width, i);
                out << buf << lines[i - 1] << "\n";
            }
        } else {
            for (long i = start; i <= end && i <= (long)lines.size(); ++i) {
                out << lines[i - 1] << "\n";
            }
        }
        entry.set("lines", Json::Num((double)lines.size()));
        entry.set("c", Json::Str(out.str()));
        filesOut.arr.push_back(entry);
    }
    Json out = Json::Obj();
    out.set("r", filesOut);
    return out;
}

// ===========================================================================
// Tool: multi_patch_tool  (alias: patch)
// ===========================================================================
Json toolMultiPatch(const Json& args, const fs::path& baseDir) {
    try {
        const std::vector<Json>* patches = args.getArr2("p", "patches");
        if (!patches || patches->empty()) {
            Json r = Json::Obj();
            r.set("err", Json::Str("'p' array is required"));
            return r;
        }
        bool preview = args.getBool2("preview", "preview", false);
        // Agent-safe default: a batch is committed only if every patch succeeds.
        // Set atomic:false when legacy partial-commit behavior is explicitly desired.
        bool atomic = args.getBool2("atomic", "transactional", true);

        struct FileState {
            std::string originalContent;
            std::string currentContent;
            std::vector<long> origins;
            std::string originalHash;
            bool backupDone = false;
        };
        std::map<std::string, FileState> fileCache;
        Json results = Json::Arr();

        for (size_t pIdx = 0; pIdx < patches->size(); ++pIdx) {
            const Json& p = (*patches)[pIdx];
            std::string relFile = p.getStr2("f", "file");
            Json entry = Json::Obj();
            entry.set("f", Json::Str(relFile));
            entry.set("idx", Json::Num((double)pIdx));

            try { // per-patch guard: one bad patch must not lose the whole batch's results

            if (relFile.empty()) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("'f' (file) is required"));
                results.arr.push_back(entry);
                continue;
            }

            fs::path full;
            std::error_code ec;
            if (!safeResolve(baseDir, relFile, full)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("path escapes project sandbox: " + relFile));
                results.arr.push_back(entry);
                continue;
            }
            if (isProtectedPath(baseDir, full)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("refusing to write to the project root or .mpt_backups"));
                results.arr.push_back(entry);
                continue;
            }

            auto it = fileCache.find(relFile);
            if (it == fileCache.end()) {
                if (!fs::exists(full, ec)) {
                    entry.set("st", Json::Str("fail"));
                    entry.set("r", Json::Str("file not found: " + relFile));
                    results.arr.push_back(entry);
                    continue;
                }
                if (fs::is_directory(full, ec)) {
                    entry.set("st", Json::Str("fail"));
                    entry.set("r", Json::Str("is a directory, not a file: " + relFile));
                    results.arr.push_back(entry);
                    continue;
                }
                std::string content = readFileAll(full.string());
                if (content.empty() && fs::file_size(full, ec) > 0) {
                    entry.set("st", Json::Str("fail"));
                    entry.set("r", Json::Str("read failed (permission or binary?)"));
                    results.arr.push_back(entry);
                    continue;
                }
                FileState fs_;
                fs_.originalContent = content;
                fs_.currentContent = content;
                auto initLines = splitLines(normalizeLineEndings(content));
                initOriginalOrigins(initLines, fs_.origins);
                fs_.originalHash = fnv1a64Hex(content);
                fileCache[relFile] = fs_;
                it = fileCache.find(relFile);
            }

            std::string& content = it->second.currentContent;
            std::vector<long>& origins = it->second.origins;
            bool echo = p.getBool2("echo", "echo", false);
            std::string mode = p.getStr2("mode", "mode", "search_replace");

            if (mode == "delete_lines" || mode == "dl") {
                long startLine = p.getInt2("s", "start", 0);
                long endLine = p.getInt2("e", "end", 0);
                if (startLine < 1) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("'s' (start line) required")); results.arr.push_back(entry); continue; }
                if (endLine < startLine) endLine = startLine;
                size_t adjStart = 0, adjEnd = 0;
                if (!currentIndexForOriginalLine(origins, startLine, adjStart) || !currentIndexForOriginalLine(origins, endLine, adjEnd)) {
                    entry.set("st", Json::Str("fail"));
                    entry.set("r", Json::Str("one or both original lines " + std::to_string(startLine) + "-" + std::to_string(endLine) + " were already removed/replaced in this batch; refusing to guess"));
                    results.arr.push_back(entry); continue;
                }
                if (adjEnd < adjStart) std::swap(adjStart, adjEnd);
                long deletedCount = (long)(adjEnd - adjStart + 1);
                auto fileLines = splitLines(normalizeLineEndings(content));
                std::string expFail;
                if (!verifyExpectedContent(p, fileLines, adjStart, adjEnd, expFail)) {
                    entry.set("st", Json::Str("fail")); entry.set("r", Json::Str(expFail));
                    results.arr.push_back(entry); continue;
                }
                if (echo) { std::ostringstream eo; for (size_t i=adjStart;i<=adjEnd && i<fileLines.size();++i) eo << (i+1) << ": " << fileLines[i] << "\n"; entry.set("echo", Json::Str(eo.str())); }
                fileLines.erase(fileLines.begin()+adjStart, fileLines.begin()+adjEnd+1);
                origins.erase(origins.begin()+adjStart, origins.begin()+adjEnd+1);
                content = joinLines(fileLines);
                entry.set("sl", Json::Num((double)startLine)); entry.set("el", Json::Num((double)endLine));
                entry.set("adj", Json::Num((double)adjStart + 1));
                if (preview) { entry.set("st", Json::Str("preview")); entry.set("preview", Json::Str("delete original " + std::to_string(startLine) + "-" + std::to_string(endLine) + " (" + std::to_string(deletedCount) + " lines)")); }
                else { entry.set("st", Json::Str("ok")); entry.set("del", Json::Num((double)deletedCount)); }
                results.arr.push_back(entry); continue;
            }

            if (mode == "replace_lines" || mode == "rl") {
                long startLine = p.getInt2("s", "start", 0);
                long endLine = p.getInt2("e", "end", 0);
                std::string newText = getTextOrB64(p, "n", "new", "nb64");
                if (startLine < 1) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("'s' (start line) required for replace_lines")); results.arr.push_back(entry); continue; }
                if (endLine < startLine) endLine = startLine;
                size_t adjStart=0, adjEnd=0;
                if (!currentIndexForOriginalLine(origins, startLine, adjStart) || !currentIndexForOriginalLine(origins, endLine, adjEnd)) {
                    entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("one or both original lines " + std::to_string(startLine) + "-" + std::to_string(endLine) + " were already removed/replaced in this batch; refusing to guess")); results.arr.push_back(entry); continue;
                }
                if (adjEnd < adjStart) std::swap(adjStart, adjEnd);
                auto fileLines = splitLines(normalizeLineEndings(content));
                std::string expFail;
                if (!verifyExpectedContent(p, fileLines, adjStart, adjEnd, expFail)) {
                    entry.set("st", Json::Str("fail")); entry.set("r", Json::Str(expFail));
                    results.arr.push_back(entry); continue;
                }
                auto newLines = splitLines(normalizeLineEndings(newText));
                long oldCount = (long)(adjEnd-adjStart+1), newCount=(long)newLines.size();
                if (echo) { std::ostringstream eo; for (size_t i=adjStart;i<adjStart+newLines.size() && i<fileLines.size();++i) eo << (i+1) << ": " << (i<adjStart+newLines.size() ? newLines[i-adjStart] : fileLines[i]) << "\n"; entry.set("echo", Json::Str(eo.str())); }
                replaceLineRange(fileLines, origins, adjStart, (size_t)oldCount, newLines);
                content = joinLines(fileLines);
                entry.set("sl", Json::Num((double)startLine)); entry.set("el", Json::Num((double)(startLine + newCount - 1)));
                entry.set("adj", Json::Num((double)adjStart + 1));
                if (preview) { entry.set("st", Json::Str("preview")); entry.set("preview", Json::Str("replace original " + std::to_string(startLine) + "-" + std::to_string(endLine))); }
                else entry.set("st", Json::Str("ok"));
                results.arr.push_back(entry); continue;
            }

            if (mode == "diff" || mode == "d") {
                std::string diffText = getTextOrB64(p, "diff", "diff", "diffb64");
                if (diffText.empty()) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("'diff' text required for diff mode")); results.arr.push_back(entry); continue; }
                auto fileLines = splitLines(normalizeLineEndings(content));
                std::vector<DiffHunk> hunks;
                try { hunks = parseUnifiedDiff(diffText); }
                catch (std::exception& diffEx) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str(std::string("malformed @@ hunk header (") + diffEx.what() + ")")); results.arr.push_back(entry); continue; }
                if (hunks.empty()) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("no valid @@ hunks found in diff text")); results.arr.push_back(entry); continue; }
                auto applyResult = applyHunksMapped(fileLines, origins, hunks);
                if (!applyResult.ok) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str(applyResult.error)); results.arr.push_back(entry); continue; }
                content = joinLines(applyResult.lines);
                origins = applyResult.origins;
                if (echo) { std::ostringstream eo; for (size_t i=0;i<applyResult.lines.size();++i) eo << (i+1) << ": " << applyResult.lines[i] << "\n"; entry.set("echo", Json::Str(eo.str())); }
                entry.set("adj", Json::Num((double)(hunks.front().oldStart > 0 ? currentLineNumberForOriginalLine(origins, hunks.front().oldStart) : 1)));
                if (preview) { entry.set("st", Json::Str("preview")); entry.set("preview", Json::Str("would apply " + std::to_string(hunks.size()) + " hunks")); }
                else { entry.set("st", Json::Str("ok")); entry.set("hunks", Json::Num((double)hunks.size())); }
                results.arr.push_back(entry); continue;
            }

            std::string oldText = getTextOrB64(p, "o", "old", "ob64");
            std::string newText = getTextOrB64(p, "n", "new", "nb64");
            if (oldText.empty()) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("'o' (old) must not be empty"));
                results.arr.push_back(entry);
                continue;
            }
            std::vector<size_t> positions;
            std::string contentForSearch = content;
            std::string oldForSearch = oldText;
            std::string matchMode = "exact";

            auto findAll = [&](const std::string& hay, const std::string& needle) {
                std::vector<size_t> pos;
                size_t cur = 0;
                while (true) {
                    size_t f = hay.find(needle, cur);
                    if (f == std::string::npos) break;
                    pos.push_back(f);
                    cur = f + std::max<size_t>(1, needle.size());
                    if (pos.size() > 10000) break;
                }
                return pos;
            };

            positions = findAll(contentForSearch, oldForSearch);
            if (positions.empty()) {
                std::string normContent = normalizeLineEndings(content);
                std::string normOld = normalizeLineEndings(oldText);
                auto normPos = findAll(normContent, normOld);
                if (!normPos.empty()) {
                    contentForSearch = normContent;
                    oldForSearch = normOld;
                    positions = normPos;
                    matchMode = "norm";
                }
            }

            bool usedFuzzy = false;
            if (positions.empty()) {
                auto fileLines = splitLines(normalizeLineEndings(content));
                auto oldLines = splitLines(normalizeLineEndings(oldText));
                long fuzzyOcc = p.getInt2("occ", "occurrence", 0);
                // Fuzzy (whitespace-insensitive) matching is the path most likely to hit
                // more than one candidate (e.g. repeated boilerplate blocks), so it gets
                // the same ambiguity/occ handling as the exact/normalized match path below
                // instead of silently taking the first hit. Returns true if this call fully
                // handled the patch (success, ambiguous-fail, or occ-out-of-range-fail).
                auto tryFuzzyMode = [&](bool trimWhitespace, const std::string& modeName) -> bool {
                    auto matches = findAllLinesSequences(fileLines, oldLines, trimWhitespace);
                    if (matches.empty()) return false;
                    usedFuzzy = true;
                    size_t startIdx;
                    if (matches.size() > 1 && fuzzyOcc == 0) {
                        entry.set("st", Json::Str("fail"));
                        std::string reason = "ambiguous fuzzy(" + modeName + ") match: " +
                                             std::to_string(matches.size()) + " matches; pass 'occ' (1-based)";
                        Json matchInfo = Json::Arr();
                        for (auto mi : matches) {
                            Json m = Json::Obj();
                            m.set("line", Json::Num((double)(mi + 1)));
                            matchInfo.arr.push_back(m);
                        }
                        entry.set("r", Json::Str(reason));
                        entry.set("matches", matchInfo);
                        results.arr.push_back(entry);
                        return true;
                    }
                    if (fuzzyOcc != 0) {
                        if (fuzzyOcc < 1 || (size_t)fuzzyOcc > matches.size()) {
                            entry.set("st", Json::Str("fail"));
                            entry.set("r", Json::Str("occ out of range; " + std::to_string(matches.size()) +
                                                     " fuzzy(" + modeName + ") matches found"));
                            results.arr.push_back(entry);
                            return true;
                        }
                        startIdx = matches[(size_t)fuzzyOcc - 1];
                    } else {
                        startIdx = matches[0];
                    }
                    matchMode = modeName;
                    auto newLines = splitLines(normalizeLineEndings(newText));
                    std::vector<std::string> resultLines;
                    resultLines.reserve(fileLines.size() - oldLines.size() + newLines.size());
                    for (size_t i=0;i<startIdx;++i) resultLines.push_back(fileLines[i]);
                    for (auto &l : newLines) resultLines.push_back(l);
                    for (size_t i=startIdx+oldLines.size(); i<fileLines.size(); ++i) resultLines.push_back(fileLines[i]);
                    if (echo) {
                        std::ostringstream echoOut;
                        for (size_t i = startIdx; i < startIdx + newLines.size() && i < resultLines.size(); ++i)
                            echoOut << (i + 1) << ": " << resultLines[i] << "\n";
                        entry.set("echo", Json::Str(echoOut.str()));
                    }
                    if (preview) {
                        entry.set("preview", Json::Str("fuzzy(" + modeName + ") at " + std::to_string(startIdx + 1)));
                        entry.set("st", Json::Str("preview"));
                    } else {
                        content = joinLines(resultLines);
                        std::vector<long> nextOrigins;
                        nextOrigins.reserve(resultLines.size());
                        for (size_t oi=0; oi<startIdx; ++oi) nextOrigins.push_back(origins[oi]);
                        for (size_t ni=0; ni<newLines.size(); ++ni) nextOrigins.push_back(ni < oldLines.size() ? origins[startIdx+ni] : 0);
                        for (size_t oi=startIdx+oldLines.size(); oi<origins.size(); ++oi) nextOrigins.push_back(origins[oi]);
                        origins.swap(nextOrigins);
                        entry.set("st", Json::Str("ok"));
                    }
                    entry.set("mode", Json::Str(matchMode));
                    entry.set("sl", Json::Num((double)(startIdx+1)));
                    entry.set("el", Json::Num((double)(startIdx+newLines.size())));
                    entry.set("adj", Json::Num((double)(startIdx + 1)));
                    results.arr.push_back(entry);
                    return true;
                };
                if (tryFuzzyMode(false, "lines")) continue;
                if (tryFuzzyMode(true, "trim")) continue;
            }

            if (!usedFuzzy && positions.empty()) {
                entry.set("st", Json::Str("fail"));
                std::string firstOldLine = oldText.substr(0, oldText.find('\n'));
                if (firstOldLine.size() > 80) firstOldLine = firstOldLine.substr(0, 80);
                std::string reason = "old text not found. first line: [" + firstOldLine + "]";
                auto fileLines = splitLines(normalizeLineEndings(content));
                std::string key = firstOldLine.substr(0, std::min<size_t>(20, firstOldLine.size()));
                for (size_t i = 0; i < fileLines.size(); ++i) {
                    if (fileLines[i].find(key) != std::string::npos) {
                        reason += "\nNear-match at line " + std::to_string(i + 1) + ": [" +
                                  fileLines[i].substr(0, std::min<size_t>(60, fileLines[i].size())) + "]";
                        break;
                    }
                }
                entry.set("r", Json::Str(reason));
                results.arr.push_back(entry);
                continue;
            }

            long occurrence = p.getInt2("occ", "occurrence", 0);
            if (positions.size() > 1 && occurrence == 0) {
                entry.set("st", Json::Str("fail"));
                std::string reason = "ambiguous: " + std::to_string(positions.size()) + " matches; pass 'occ' (1-based)";
                Json matchInfo = Json::Arr();
                for (auto ppos : positions) {
                    Json m = Json::Obj();
                    m.set("line", Json::Num((double)lineNumberAt(contentForSearch, ppos)));
                    std::string ctx = contentForSearch.substr(ppos, std::min<size_t>(40, contentForSearch.size() - ppos));
                    size_t nl = ctx.find('\n');
                    if (nl != std::string::npos) ctx = ctx.substr(0, nl);
                    m.set("ctx", Json::Str(ctx));
                    matchInfo.arr.push_back(m);
                }
                entry.set("r", Json::Str(reason));
                entry.set("matches", matchInfo);
                results.arr.push_back(entry);
                continue;
            }

            size_t chosenPos;
            if (occurrence != 0) {
                if (occurrence < 1 || (size_t)occurrence > positions.size()) {
                    entry.set("st", Json::Str("fail"));
                    entry.set("r", Json::Str("occ out of range; " + std::to_string(positions.size()) + " found"));
                    results.arr.push_back(entry);
                    continue;
                }
                chosenPos = positions[occurrence - 1];
            } else chosenPos = positions[0];

            long startLine = lineNumberAt(contentForSearch, chosenPos);
            std::string baseContent = contentForSearch;
            std::string newContent;
            if (matchMode == "norm") {
                newContent = baseContent.substr(0, chosenPos) + normalizeLineEndings(newText) +
                             baseContent.substr(chosenPos + oldForSearch.size());
            } else {
                if (contentForSearch.size() == content.size() && matchMode=="exact") {
                    newContent = content.substr(0, chosenPos) + newText +
                                 content.substr(chosenPos + oldText.size());
                } else {
                    newContent = baseContent.substr(0, chosenPos) + newText +
                                 baseContent.substr(chosenPos + oldForSearch.size());
                }
            }
            if (echo) {
                auto afterLines = splitLines(normalizeLineEndings(newContent));
                long newLineCount = (long)std::count(newText.begin(), newText.end(), '\n') + 1;
                std::ostringstream echoOut;
                for (long i = startLine; i < startLine + newLineCount && i <= (long)afterLines.size(); ++i)
                    echoOut << i << ": " << afterLines[i - 1] << "\n";
                entry.set("echo", Json::Str(echoOut.str()));
            }
            if (preview) {
                // Still update the in-memory model so later patches in this preview batch
                // see exactly the same hypothetical file as they would after a real run.
                content = newContent;
                size_t originStartPreview = (size_t)std::max<long>(0, lineNumberAt(contentForSearch, chosenPos) - 1);
                auto replacementLinesPreview = splitLines(normalizeLineEndings(newText));
                size_t oldLineCountPreview = splitLines(normalizeLineEndings(oldForSearch)).size();
                if (originStartPreview < origins.size() && originStartPreview + oldLineCountPreview <= origins.size()) {
                    std::vector<long> nextOrigins;
                    for (size_t oi=0; oi<originStartPreview; ++oi) nextOrigins.push_back(origins[oi]);
                    for (size_t ni=0; ni<replacementLinesPreview.size(); ++ni) nextOrigins.push_back(ni < oldLineCountPreview ? origins[originStartPreview+ni] : 0);
                    for (size_t oi=originStartPreview+oldLineCountPreview; oi<origins.size(); ++oi) nextOrigins.push_back(origins[oi]);
                    origins.swap(nextOrigins);
                }
                std::ostringstream pv;
                pv << "replace at line " << startLine;
                entry.set("preview", Json::Str(pv.str()));
                entry.set("st", Json::Str("preview"));
            } else {
                content = newContent;
                // chosenPos is in the current pre-edit content, so its current line index
                // is the exact origin-vector slot to replace. Do not treat the line number
                // as an original coordinate after earlier mutations in the same batch.
                size_t originStart = (size_t)std::max<long>(0, lineNumberAt(contentForSearch, chosenPos) - 1);
                auto replacementLines = splitLines(normalizeLineEndings(newText));
                size_t oldLineCount = splitLines(normalizeLineEndings(oldForSearch)).size();
                if (originStart < origins.size() && originStart + oldLineCount <= origins.size()) {
                    std::vector<long> nextOrigins;
                    for (size_t oi=0; oi<originStart; ++oi) nextOrigins.push_back(origins[oi]);
                    for (size_t ni=0; ni<replacementLines.size(); ++ni) nextOrigins.push_back(ni < oldLineCount ? origins[originStart+ni] : 0);
                    for (size_t oi=originStart+oldLineCount; oi<origins.size(); ++oi) nextOrigins.push_back(origins[oi]);
                    origins.swap(nextOrigins);
                }
                entry.set("st", Json::Str("ok"));
            }
            entry.set("mode", Json::Str(matchMode));
            entry.set("sl", Json::Num((double)startLine));
            entry.set("el", Json::Num((double)(startLine + (long)std::count(newText.begin(), newText.end(), '\n'))));
            entry.set("adj", Json::Num((double)(startLine + 1)));
            results.arr.push_back(entry);

            } catch (std::exception& e) { // per-patch guard close
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str(std::string("exception: ") + e.what()));
                results.arr.push_back(entry);
            } catch (...) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("unknown exception processing this patch"));
                results.arr.push_back(entry);
            }
        }

        Json filesSummary = Json::Arr();
        bool batchFailed = false;
        for (const auto& rr : results.arr) {
            if (rr.getStr2("st", "status") == "fail") { batchFailed = true; break; }
        }

        if (!preview && atomic && batchFailed) {
            for (auto& rr : results.arr) {
                if (rr.getStr2("st", "status") == "ok") {
                    rr.set("st", Json::Str("aborted"));
                    rr.set("r", Json::Str("batch aborted because another patch failed; nothing was written (atomic:true)"));
                }
            }
            Json out = Json::Obj();
            out.set("r", results);
            out.set("atomic", Json::Bool(true));
            out.set("aborted", Json::Bool(true));
            return out;
        }

        if (!preview) {
            // Preflight every dirty file before changing any file on disk. This prevents
            // an LLM batch from clobbering a file that another process edited meanwhile.
            for (auto& [relFile, state] : fileCache) {
                if (state.currentContent == state.originalContent) continue;
                fs::path full;
                std::error_code ec;
                if (!safeResolve(baseDir, relFile, full)) {
                    batchFailed = true;
                    break;
                }
                std::string disk = readFileAll(full.string());
                if (fnv1a64Hex(disk) != state.originalHash) {
                    batchFailed = true;
                    for (auto& rr : results.arr) {
                        if (rr.getStr2("f", "") == relFile && rr.getStr2("st", "") == "ok") {
                            rr.set("st", Json::Str("fail"));
                            rr.set("r", Json::Str("file changed on disk after batch read; refusing to overwrite external changes"));
                        }
                    }
                }
            }
            if (atomic && batchFailed) {
                for (auto& rr : results.arr) {
                    if (rr.getStr2("st", "status") == "ok") {
                        rr.set("st", Json::Str("aborted"));
                        rr.set("r", Json::Str("batch aborted during preflight; nothing was written (atomic:true)"));
                    }
                }
                Json out = Json::Obj();
                out.set("r", results);
                out.set("atomic", Json::Bool(true));
                out.set("aborted", Json::Bool(true));
                return out;
            }

            std::vector<std::pair<std::string, std::string>> written;
            bool writeFailed = false;
            for (auto& [relFile, state] : fileCache) {
                if (state.currentContent == state.originalContent) continue;
                fs::path full;
                std::error_code ec;
                if (!safeResolve(baseDir, relFile, full)) { writeFailed = true; break; }
                if (!state.backupDone) {
                    backupFile(baseDir, relFile, state.originalContent);
                    state.backupDone = true;
                }
                fs::create_directories(full.parent_path(), ec);
                Json fentry = Json::Obj();
                fentry.set("f", Json::Str(relFile));
                if (!writeFileAll(full.string(), state.currentContent)) {
                    writeFailed = true;
                    fentry.set("written", Json::Bool(false));
                    fentry.set("r", Json::Str("write failed"));
                    filesSummary.arr.push_back(fentry);
                    break;
                }
                written.push_back({relFile, state.originalContent});
                fentry.set("written", Json::Bool(true));
                fentry.set("chk", Json::Str(fnv1a64Hex(state.currentContent)));
                fentry.set("lines", Json::Num((double)splitLines(normalizeLineEndings(state.currentContent)).size()));
                fentry.set("bytes", Json::Num((double)state.currentContent.size()));
                filesSummary.arr.push_back(fentry);
            }
            if (writeFailed && atomic) {
                // Best-effort rollback of files already committed by this batch.
                for (auto it = written.rbegin(); it != written.rend(); ++it) {
                    fs::path full;
                    if (safeResolve(baseDir, it->first, full)) writeFileAll(full.string(), it->second);
                }
                for (auto& rr : results.arr) {
                    if (rr.getStr2("st", "status") == "ok") {
                        rr.set("st", Json::Str("aborted"));
                        rr.set("r", Json::Str("batch write failed; previously written files were rolled back (atomic:true)"));
                    }
                }
                Json out = Json::Obj();
                out.set("r", results);
                out.set("files", filesSummary);
                out.set("atomic", Json::Bool(true));
                out.set("aborted", Json::Bool(true));
                return out;
            }
            if (writeFailed && !atomic) {
                for (auto& rr : results.arr) {
                    if (rr.getStr2("f", "") != "" && rr.getStr2("st", "") == "ok") {
                        // Keep per-file result detail; only the failing file is known here.
                    }
                }
            }
        }

        Json out = Json::Obj();
        out.set("r", results);
        if (preview) out.set("preview", Json::Bool(true));
        else { out.set("files", filesSummary); out.set("atomic", Json::Bool(atomic)); }
        return out;
    } catch (std::exception& e) {
        Json r = Json::Obj();
        r.set("err", Json::Str(std::string("exception: ") + e.what()));
        Json res = Json::Arr();
        Json entry = Json::Obj();
        entry.set("st", Json::Str("fail"));
        entry.set("r", Json::Str(e.what()));
        res.arr.push_back(entry);
        r.set("r", res);
        return r;
    } catch (...) {
        Json r = Json::Obj();
        r.set("err", Json::Str("unknown exception"));
        return r;
    }
}

// ===========================================================================
// Tool: file_edit_tool  (alias: edit)  — with offset tracking (FIXED)
// ===========================================================================
Json toolFileEdit(const Json& args, const fs::path& baseDir) {
    const std::vector<Json>* edits = args.getArr2("e", "edits");
    if (!edits || edits->empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("'e' array is required"));
        return r;
    }
    // Agent-safe default, mirroring patch's atomic semantics: the whole batch
    // commits only if every edit succeeds and no dirty file changed on disk
    // since it was read. Set atomic:false for legacy partial-commit behavior.
    bool atomic = args.getBool2("atomic", "transactional", true);

    struct FileState {
        std::string originalContent;
        std::vector<std::string> lines;
        std::vector<long> origins;
        bool dirty = false;
        bool backupDone = false;
        bool existedOnDisk = false; // false only for files newly created by this batch
        std::string originalHash;   // fnv1a64Hex(originalContent); meaningful only if existedOnDisk
    };
    std::map<std::string, FileState> fileCache;
    Json results = Json::Arr();

    for (size_t eIdx = 0; eIdx < edits->size(); ++eIdx) {
        const Json& e = (*edits)[eIdx];
        std::string relFile = e.getStr2("f", "file");
        std::string mode = e.getStr2("mode", "mode");
        std::string content = getTextOrB64(e, "c", "content", "cb64");
        Json entry = Json::Obj();
        entry.set("f", Json::Str(relFile));
        entry.set("idx", Json::Num((double)eIdx));
        fs::path full;
        std::error_code ec;

        try { // per-edit guard: one bad edit must not lose the whole batch's results

        if (relFile.empty()) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("'f' (file) required"));
            results.arr.push_back(entry);
            continue;
        }
        if (!safeResolve(baseDir, relFile, full)) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("path escapes project sandbox: " + relFile));
            results.arr.push_back(entry);
            continue;
        }
        if (isProtectedPath(baseDir, full)) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("refusing to write to the project root or .mpt_backups"));
            results.arr.push_back(entry);
            continue;
        }

        if (mode == "create" || mode == "c") {
            bool overwrite = e.getBool2("ow", "overwrite", false);
            auto cit = fileCache.find(relFile);
            if (cit == fileCache.end()) {
                if (fs::exists(full, ec) && fs::is_directory(full, ec)) {
                    entry.set("st", Json::Str("fail"));
                    entry.set("r", Json::Str("a directory already exists at this path"));
                    results.arr.push_back(entry);
                    continue;
                }
                bool existed = fs::exists(full, ec);
                if (existed && !overwrite) {
                    entry.set("st", Json::Str("fail"));
                    entry.set("r", Json::Str("exists (ow:true to replace)"));
                    results.arr.push_back(entry);
                    continue;
                }
                FileState st;
                st.existedOnDisk = existed;
                if (existed) {
                    st.originalContent = readFileAll(full.string());
                    st.originalHash = fnv1a64Hex(st.originalContent);
                }
                fileCache[relFile] = st;
                cit = fileCache.find(relFile);
            }
            // Batch-local re-create (this file was already claimed earlier in the
            // same batch) always overwrites the in-memory content; nothing has
            // reached disk yet, so 'ow' only gates the first touch above.
            cit->second.lines = splitLines(normalizeLineEndings(content));
            initOriginalOrigins(cit->second.lines, cit->second.origins);
            cit->second.dirty = true;
            entry.set("st", Json::Str("ok"));
            entry.set("n", Json::Num((double)cit->second.lines.size()));
            results.arr.push_back(entry);
            continue;
        }

        auto it = fileCache.find(relFile);
        if (it == fileCache.end()) {
            if (!fs::exists(full, ec)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("not found (use mode:create for new files)"));
                results.arr.push_back(entry);
                continue;
            }
            if (fs::is_directory(full, ec)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("is a directory, not a file: " + relFile));
                results.arr.push_back(entry);
                continue;
            }
            FileState st;
            st.existedOnDisk = true;
            st.originalContent = readFileAll(full.string());
            st.originalHash = fnv1a64Hex(st.originalContent);
            st.lines = splitLines(normalizeLineEndings(st.originalContent));
            initOriginalOrigins(st.lines, st.origins);
            fileCache[relFile] = st;
            it = fileCache.find(relFile);
        }
        std::vector<std::string>& lines = it->second.lines;
        std::vector<long>& origins = it->second.origins;

        if (mode == "append" || mode == "prepend" || mode == "a" || mode == "pp") {
            bool isAppend = (mode == "append" || mode == "a");
            std::vector<std::string> newLines = splitLines(normalizeLineEndings(content));
            if (isAppend) {
                lines.insert(lines.end(), newLines.begin(), newLines.end());
                origins.insert(origins.end(), newLines.size(), 0);
            } else {
                lines.insert(lines.begin(), newLines.begin(), newLines.end());
                origins.insert(origins.begin(), newLines.size(), 0);
            }
            it->second.dirty = true;
            entry.set("st", Json::Str("ok"));
            entry.set("n", Json::Num((double)lines.size()));
            entry.set("adj", Json::Num((double)lines.size()));
        } else if (mode == "insert_after" || mode == "insert_before" || mode == "ia" || mode == "ib") {
            bool isAfter = (mode == "insert_after" || mode == "ia");
            long lineNum = e.getInt2("l", "line", -1);
            size_t targetIndex = 0;
            if (!currentIndexForOriginalLine(origins, lineNum, targetIndex)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("original line " + std::to_string(lineNum) + " is not present (it may have been removed in this batch)"));
                results.arr.push_back(entry);
                continue;
            }
            std::string expFail;
            if (!verifyExpectedContent(e, lines, targetIndex, targetIndex, expFail)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str(expFail));
                results.arr.push_back(entry);
                continue;
            }
            size_t insertPos = isAfter ? targetIndex + 1 : targetIndex;
            std::vector<std::string> newLines = splitLines(normalizeLineEndings(content));
            lines.insert(lines.begin() + (long)insertPos, newLines.begin(), newLines.end());
            origins.insert(origins.begin() + (long)insertPos, newLines.size(), 0);
            it->second.dirty = true;
            entry.set("st", Json::Str("ok"));
            entry.set("n", Json::Num((double)lines.size()));
            entry.set("adj", Json::Num((double)insertPos + 1));
        } else if (mode == "delete" || mode == "del") {
            long startLine = e.getInt2("s", "start", 0);
            long endLine = e.getInt2("e", "end", 0);
            if (startLine < 1) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("'s' (start) required"));
                results.arr.push_back(entry);
                continue;
            }
            if (endLine < startLine) endLine = startLine;
            size_t adjStart=0, adjEnd=0;
            if (!currentIndexForOriginalLine(origins, startLine, adjStart) || !currentIndexForOriginalLine(origins, endLine, adjEnd)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("one or both original lines " + std::to_string(startLine) + "-" + std::to_string(endLine) + " are no longer present; refusing to guess"));
                results.arr.push_back(entry);
                continue;
            }
            if (adjEnd < adjStart) std::swap(adjStart, adjEnd);
            std::string expFail;
            if (!verifyExpectedContent(e, lines, adjStart, adjEnd, expFail)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str(expFail));
                results.arr.push_back(entry);
                continue;
            }
            long deleted = (long)(adjEnd - adjStart + 1);
            lines.erase(lines.begin()+adjStart, lines.begin()+adjEnd+1);
            origins.erase(origins.begin()+adjStart, origins.begin()+adjEnd+1);
            it->second.dirty = true;
            entry.set("st", Json::Str("ok"));
            entry.set("del", Json::Num((double)deleted));
            entry.set("adj", Json::Num((double)adjStart + 1));
        } else {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("unknown mode: " + mode +
                     " (use create/append/prepend/insert_after/insert_before/delete)"));
        }
        results.arr.push_back(entry);

        } catch (std::exception& ex) { // per-edit guard close
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str(std::string("exception: ") + ex.what()));
            results.arr.push_back(entry);
        } catch (...) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("unknown exception processing this edit"));
            results.arr.push_back(entry);
        }
    }

    bool batchFailed = false;
    for (const auto& rr : results.arr) {
        if (rr.getStr2("st", "status") == "fail") { batchFailed = true; break; }
    }

    if (atomic && batchFailed) {
        for (auto& rr : results.arr) {
            if (rr.getStr2("st", "status") == "ok") {
                rr.set("st", Json::Str("aborted"));
                rr.set("r", Json::Str("batch aborted because another edit failed; nothing was written (atomic:true)"));
            }
        }
        Json out = Json::Obj();
        out.set("r", results);
        out.set("atomic", Json::Bool(true));
        out.set("aborted", Json::Bool(true));
        return out;
    }

    // Preflight every dirty pre-existing file before changing anything on disk.
    // This prevents a batch from clobbering a file another process edited
    // meanwhile. Freshly-created files (existedOnDisk == false) have nothing to
    // compare against and are skipped here.
    for (auto& [relFile, state] : fileCache) {
        if (!state.dirty || !state.existedOnDisk) continue;
        fs::path full;
        if (!safeResolve(baseDir, relFile, full)) { batchFailed = true; continue; }
        std::string disk = readFileAll(full.string());
        if (fnv1a64Hex(disk) != state.originalHash) {
            batchFailed = true;
            for (auto& rr : results.arr) {
                if (rr.getStr2("f", "") == relFile && rr.getStr2("st", "") == "ok") {
                    rr.set("st", Json::Str("fail"));
                    rr.set("r", Json::Str("file changed on disk after batch read; refusing to overwrite external changes"));
                }
            }
        }
    }
    if (atomic && batchFailed) {
        for (auto& rr : results.arr) {
            if (rr.getStr2("st", "status") == "ok") {
                rr.set("st", Json::Str("aborted"));
                rr.set("r", Json::Str("batch aborted during preflight; nothing was written (atomic:true)"));
            }
        }
        Json out = Json::Obj();
        out.set("r", results);
        out.set("atomic", Json::Bool(true));
        out.set("aborted", Json::Bool(true));
        return out;
    }

    Json filesSummary = Json::Arr();
    std::vector<std::pair<std::string, FileState*>> written; // for rollback
    bool writeFailed = false;
    for (auto& [relFile, state] : fileCache) {
        if (!state.dirty) continue;
        fs::path full;
        std::error_code ec2;
        if (!safeResolve(baseDir, relFile, full)) { writeFailed = true; break; }
        if (state.existedOnDisk && !state.backupDone) {
            backupFile(baseDir, relFile, state.originalContent);
            state.backupDone = true;
        }
        fs::create_directories(full.parent_path(), ec2);
        std::string finalContent = joinLines(state.lines);
        Json fentry = Json::Obj();
        fentry.set("f", Json::Str(relFile));
        if (!writeFileAll(full.string(), finalContent)) {
            writeFailed = true;
            for (auto& r : results.arr) {
                if (r.getStr2("f","") == relFile && r.getStr2("st","") == "ok") {
                    r.set("st", Json::Str("fail"));
                    r.set("r", Json::Str("write failed"));
                }
            }
            fentry.set("written", Json::Bool(false));
            filesSummary.arr.push_back(fentry);
            if (atomic) break; else continue;
        }
        written.push_back({relFile, &state});
        fentry.set("written", Json::Bool(true));
        fentry.set("chk", Json::Str(fnv1a64Hex(finalContent)));
        fentry.set("lines", Json::Num((double)state.lines.size()));
        fentry.set("bytes", Json::Num((double)finalContent.size()));
        filesSummary.arr.push_back(fentry);
    }

    if (writeFailed && atomic) {
        // Best-effort rollback of files already committed by this batch: restore
        // original content for files that existed before, and delete files this
        // batch newly created (mode:create) so a partial atomic batch leaves no trace.
        for (auto rit = written.rbegin(); rit != written.rend(); ++rit) {
            fs::path full;
            if (!safeResolve(baseDir, rit->first, full)) continue;
            if (rit->second->existedOnDisk) {
                writeFileAll(full.string(), rit->second->originalContent);
            } else {
                std::error_code rec;
                fs::remove(full, rec);
            }
        }
        for (auto& rr : results.arr) {
            if (rr.getStr2("st", "status") == "ok") {
                rr.set("st", Json::Str("aborted"));
                rr.set("r", Json::Str("batch write failed; previously written files were rolled back (atomic:true)"));
            }
        }
        Json out = Json::Obj();
        out.set("r", results);
        out.set("files", filesSummary);
        out.set("atomic", Json::Bool(true));
        out.set("aborted", Json::Bool(true));
        return out;
    }

    Json out = Json::Obj();
    out.set("r", results);
    out.set("files", filesSummary);
    out.set("atomic", Json::Bool(atomic));
    return out;
}

// ===========================================================================
// Tool: git_tool  (alias: git)  — with timeout
// ===========================================================================
Json toolGit(const Json& args, const fs::path& baseDir) {
    std::string action = args.getStr2("a", "action");
    std::string cmd;
    if (action.empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("'a' (action) required"));
        return r;
    }
    if (action == "status" || action == "st") cmd = "git status --short";
    else if (action == "diff" || action == "d") {
        std::string f = args.getStr2("f", "file");
        cmd = f.empty() ? "git diff" : ("git diff -- " + shellQuote(f));
    } else if (action == "log" || action == "l") {
        long n = args.getInt2("n", "n", 10);
        cmd = "git log --oneline -n " + std::to_string(n);
    } else if (action == "commit" || action == "ci") {
        std::string m = args.getStr2("m", "message");
        if (m.empty()) {
            Json r = Json::Obj(); r.set("err", Json::Str("'m' required for commit")); return r;
        }
        cmd = "git add -A && git commit -m " + shellQuote(m);
    } else if (action == "revert_file" || action == "rv") {
        std::string f = args.getStr2("f", "file");
        if (f.empty()) {
            Json r = Json::Obj(); r.set("err", Json::Str("'f' required for revert_file")); return r;
        }
        cmd = "git checkout -- " + shellQuote(f);
    } else if (action == "undo_last_commit" || action == "uc") cmd = "git reset --soft HEAD~1";
    else if (action == "branch" || action == "b") {
        std::string name = args.getStr2("name", "name");
        cmd = name.empty() ? "git branch" : ("git checkout -b " + shellQuote(name));
    } else if (action == "raw" || action == "r") {
        std::string raw = args.getStr2("raw", "raw_args");
        if (raw.empty()) {
            Json r = Json::Obj(); r.set("err", Json::Str("'raw' required for raw")); return r;
        }
        cmd = "git " + raw;
    } else {
        Json r = Json::Obj();
        r.set("err", Json::Str("unknown action: '" + action + "' (use st/d/l/ci/rv/uc/b/r)"));
        return r;
    }
    long timeout = args.getInt2("to", "timeout", 30);
    if (timeout < 1) timeout = 30;
    fs::path cwd = baseDir;
    std::string dirArg = args.getStr2("dir", "cwd", "");
    if (!dirArg.empty()) {
        fs::path resolved;
        if (!safeResolve(baseDir, dirArg, resolved)) {
            Json r = Json::Obj();
            r.set("err", Json::Str("'dir' is outside all registered workspaces: " + dirArg));
            return r;
        }
        std::error_code dec;
        fs::path canon = fs::canonical(resolved, dec);
        if (dec || !fs::is_directory(canon, dec) || !isRegisteredRoot(baseDir, canon)) {
            Json r = Json::Obj();
            r.set("err", Json::Str("'dir' must be a registered workspace root, got: " + dirArg));
            return r;
        }
        cwd = canon;
    }
    auto [code, output] = runShellTimed(cmd, cwd, timeout);
    Json r = Json::Obj();
    r.set("rc", Json::Num(code));
    if (code == -1000) r.set("err", Json::Str("timeout"));
    r.set("out", Json::Str(truncateOutput(output)));
    return r;
}

// ===========================================================================
// Tool: list_tool, find_tool, outline_tool, recent_tool
// ===========================================================================
std::string humanSize(uintmax_t b) {
    if (b < 1024) return std::to_string(b) + "B";
    if (b < 1024*1024) return std::to_string(b/1024) + "K";
    if (b < 1024ULL*1024*1024) return std::to_string(b/(1024*1024)) + "M";
    return std::to_string(b/(1024ULL*1024*1024)) + "G";
}
void listDirTree(const fs::path& dir, const fs::path& baseDir, int depth, int maxDepth,
                 std::ostringstream& out, const std::string& prefix, long& count, long maxCount) {
    if (maxDepth >= 0 && depth > maxDepth) return;
    if (count >= maxCount) return;
    std::error_code ec;
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        entries.push_back(e.path());
    if (ec) return;
    std::sort(entries.begin(), entries.end(), [](const fs::path& a, const fs::path& b) {
        std::error_code ec; bool aD = fs::is_directory(a, ec), bD = fs::is_directory(b, ec);
        if (aD != bD) return aD;
        return a.filename().string() < b.filename().string();
    });
    for (size_t i = 0; i < entries.size(); ++i) {
        if (count >= maxCount) { out << prefix << "  ...\n"; return; }
        const auto& entry = entries[i];
        std::string name = entry.filename().string();
        bool isLast = (i == entries.size() - 1);
        std::string branch = isLast ? "└─ " : "├─ ";
        std::string childPrefix = prefix + (isLast ? "   " : "│  ");
        std::error_code rec;
        if (fs::is_directory(entry, rec)) {
            if (isIgnoredDir(name)) continue;
            out << prefix << branch << name << "/\n"; count++;
            listDirTree(entry, baseDir, depth+1, maxDepth, out, childPrefix, count, maxCount);
        } else if (fs::is_regular_file(entry, rec)) {
            auto sz = fs::file_size(entry, ec); // bin files stay listed (tagged below), no longer hidden
            std::string szStr = ec ? "?" : humanSize(sz);
            out << prefix << branch << name << " (" << szStr << ((isBinaryExt(entry) || looksBinaryFile(entry)) ? ", bin" : "") << ")\n"; count++;
        }
    }
}
Json toolList(const Json& args, const fs::path& baseDir) {
    std::string subPath = args.getStr2("p", "path", "");
    long maxDepth = args.getInt2("depth", "depth", 3);
    long maxCount = args.getInt2("max", "max_files", 200);
    fs::path target = baseDir;
    if (!subPath.empty()) {
        if (!safeResolve(baseDir, subPath, target)) {
            Json r = Json::Obj(); r.set("err", Json::Str("path escapes project sandbox: " + subPath)); return r;
        }
        std::error_code ec;
        if (!fs::exists(target, ec) || !fs::is_directory(target, ec)) {
            Json r = Json::Obj(); r.set("err", Json::Str("dir not found: " + subPath)); return r;
        }
    }
    std::ostringstream out; long count = 0;
    std::error_code ec;
    std::string rootName = fs::relative(target, baseDir, ec).string();
    if (ec || rootName.empty() || rootName == ".") rootName = ".";
    out << rootName << "/\n"; count++;
    listDirTree(target, baseDir, 1, (int)maxDepth, out, "", count, maxCount);
    Json r = Json::Obj();
    r.set("n", Json::Num((double)count));
    if (count >= maxCount) r.set("trunc", Json::Bool(true));
    r.set("tree", Json::Str(out.str()));
    return r;
}
bool globMatch(const std::string& pattern, const std::string& text) {
    size_t p=0, t=0, starPos=std::string::npos, matchPos=0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) { p++; t++; }
        else if (p < pattern.size() && pattern[p] == '*') { starPos = p; matchPos = t; p++; }
        else if (starPos != std::string::npos) { p = starPos + 1; matchPos++; t = matchPos; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}
void findFilesRecursive(const fs::path& dir, const fs::path& baseDir, const std::string& glob,
                        std::vector<std::string>& results, long& count, long maxCount) {
    if (count >= maxCount) return;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        if (count >= maxCount) return;
        std::error_code rec;
        if (entry.is_directory(rec)) {
            if (!rec && !isIgnoredDir(entry.path().filename().string()))
                findFilesRecursive(entry.path(), baseDir, glob, results, count, maxCount);
        } else if (entry.is_regular_file(rec) && !rec) {
            std::string fname = entry.path().filename().string();
            if (globMatch(glob, fname)) {
                std::string relName = fs::relative(entry.path(), baseDir, ec).string();
                if (ec) relName = entry.path().string();
                results.push_back(relName); count++;
            }
        }
    }
}
Json toolFind(const Json& args, const fs::path& baseDir) {
    std::vector<std::string> globs;
    auto addGlob = [&](std::string s) {
        if (s.empty()) return;
        if (s[0] == '.' && s.find('*') == std::string::npos && s.find('?') == std::string::npos) {
            s = "*" + s;
        }
        globs.push_back(s);
    };

    const std::vector<Json>* globArr = args.getArr2("g", "globs");
    if (!globArr) globArr = args.getArr2("glob", "globs");
    if (globArr) {
        for (auto& g : *globArr) {
            if (g.type == Json::Type::String) addGlob(g.str);
        }
    }
    std::string singleGlob = args.getStr2("g", "glob", "");
    if (!singleGlob.empty()) addGlob(singleGlob);

    if (globs.empty()) {
        Json r = Json::Obj(); r.set("err", Json::Str("'g' or 'glob' pattern is required")); return r;
    }
    const std::vector<Json>* paths = args.getArr2("paths", "paths");
    std::vector<std::string> subpaths;
    if (paths) for (auto& p : *paths) if (p.type == Json::Type::String) subpaths.push_back(p.str);
    long maxCount = args.getInt2("max", "max_files", 200);
    std::vector<std::string> results; long count = 0;
    std::vector<std::string> missing;
    if (subpaths.empty()) {
        for (auto& g : globs) findFilesRecursive(baseDir, baseDir, g, results, count, maxCount);
    } else {
        for (auto& sp : subpaths) {
            fs::path full;
            if (!safeResolve(baseDir, sp, full)) { missing.push_back(sp + " (outside project sandbox)"); continue; }
            std::error_code ec;
            if (!fs::exists(full, ec)) { missing.push_back(sp); continue; }
            if (fs::is_directory(full, ec)) {
                for (auto& g : globs) findFilesRecursive(full, baseDir, g, results, count, maxCount);
            } else if (fs::is_regular_file(full, ec)) {
                std::string fname = full.filename().string();
                for (auto& g : globs) if (globMatch(g, fname)) {
                    std::string relName = fs::relative(full, baseDir, ec).string();
                    if (ec) relName = full.string();
                    results.push_back(relName); count++;
                }
            }
        }
    }
    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());
    Json arr = Json::Arr();
    for (auto& r : results) arr.arr.push_back(Json::Str(r));
    Json r = Json::Obj();
    r.set("n", Json::Num((double)results.size()));
    if (count >= maxCount) r.set("trunc", Json::Bool(true));
    if (!missing.empty()) {
        Json marr = Json::Arr();
        for (auto& m : missing) marr.arr.push_back(Json::Str(m));
        r.set("missing_paths", marr);
    }
    r.set("files", arr);
    return r;
}
struct OutlineEntry { long line; std::string type; std::string signature; };
std::vector<OutlineEntry> extractOutline(const std::vector<std::string>& lines, const std::string& ext) {
    std::vector<OutlineEntry> entries;
    bool isPython = (ext == ".py" || ext == ".pyw" || ext == ".pyi");
    bool isJs = (ext == ".js" || ext == ".ts" || ext == ".jsx" || ext == ".tsx" || ext == ".mjs" || ext == ".cjs");
    bool isCpp = (ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c" ||
                  ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx");
    bool isGo = (ext == ".go");
    bool isRust = (ext == ".rs");
    bool isJava = (ext == ".java" || ext == ".kt" || ext == ".scala" || ext == ".cs");
    for (size_t i = 0; i < lines.size() && i < 50000; ++i) {
        std::string line = lines[i];
        std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '/' || trimmed[0] == '#') {
            if (isPython && (trimmed.substr(0, 6) == "import" || trimmed.substr(0, 4) == "from")) {
                entries.push_back({(long)(i+1), "import", trimmed}); continue;
            }
            continue;
        }
        if (isPython) {
            if (trimmed.substr(0, 4) == "def " || trimmed.substr(0, 10) == "async def " || trimmed.find(" def ") != std::string::npos) {
                size_t defPos = trimmed.find("def ");
                if (defPos != std::string::npos) {
                    size_t colonPos = trimmed.find(':');
                    std::string sig = (colonPos != std::string::npos) ? trimmed.substr(0, colonPos) : trimmed;
                    entries.push_back({(long)(i+1), "def", sig});
                }
                continue;
            }
            if (trimmed.substr(0, 6) == "class ") {
                size_t colonPos = trimmed.find(':');
                std::string sig = (colonPos != std::string::npos) ? trimmed.substr(0, colonPos) : trimmed;
                entries.push_back({(long)(i+1), "class", sig}); continue;
            }
        }
        if (isJs) {
            if (trimmed.find("function ") != std::string::npos || trimmed.find("function(") != std::string::npos) {
                size_t bracePos = trimmed.find('{');
                std::string sig = (bracePos != std::string::npos) ? trim(trimmed.substr(0, bracePos)) : trimmed;
                if (!sig.empty()) entries.push_back({(long)(i+1), "func", sig});
                continue;
            }
            if ((trimmed.substr(0, 6) == "const " || trimmed.substr(0, 4) == "let " || trimmed.substr(0, 4) == "var " ||
                 trimmed.substr(0, 13) == "export const " || trimmed.substr(0, 11) == "export let ") &&
                (trimmed.find("=>") != std::string::npos || trimmed.find("function") != std::string::npos)) {
                size_t eqPos = trimmed.find('=');
                std::string sig = (eqPos != std::string::npos) ? trim(trimmed.substr(0, eqPos)) : trimmed;
                if (!sig.empty()) entries.push_back({(long)(i+1), "func", sig});
                continue;
            }
            if (trimmed.find("class ") != std::string::npos || trimmed.find("interface ") != std::string::npos ||
                trimmed.find("type ") != std::string::npos || trimmed.find("enum ") != std::string::npos) {
                size_t bracePos = trimmed.find('{');
                size_t eqPos = trimmed.find('=');
                size_t endPos = std::min(bracePos, eqPos);
                std::string sig = (endPos != std::string::npos) ? trim(trimmed.substr(0, endPos)) : trimmed;
                std::string typeTag = (trimmed.find("class") != std::string::npos) ? "class" : "type";
                entries.push_back({(long)(i+1), typeTag, sig}); continue;
            }
        }
        if (isRust) {
            if (trimmed.find("fn ") != std::string::npos) {
                size_t bracePos = trimmed.find('{');
                std::string sig = (bracePos != std::string::npos) ? trim(trimmed.substr(0, bracePos)) : trimmed;
                entries.push_back({(long)(i+1), "func", sig}); continue;
            }
            if (trimmed.find("struct ") != std::string::npos || trimmed.find("enum ") != std::string::npos ||
                trimmed.find("trait ") != std::string::npos || trimmed.find("impl ") != std::string::npos) {
                size_t bracePos = trimmed.find('{');
                std::string sig = (bracePos != std::string::npos) ? trim(trimmed.substr(0, bracePos)) : trimmed;
                std::string typeTag = (trimmed.find("struct") != std::string::npos) ? "struct" :
                                      (trimmed.find("trait") != std::string::npos) ? "trait" :
                                      (trimmed.find("impl") != std::string::npos) ? "impl" : "enum";
                entries.push_back({(long)(i+1), typeTag, sig}); continue;
            }
        }
        if (isGo) {
            if (trimmed.substr(0, 5) == "func ") {
                size_t bracePos = trimmed.find('{');
                std::string sig = (bracePos != std::string::npos) ? trim(trimmed.substr(0, bracePos)) : trimmed;
                entries.push_back({(long)(i+1), "func", sig}); continue;
            }
            if (trimmed.substr(0, 5) == "type " && (trimmed.find("struct") != std::string::npos || trimmed.find("interface") != std::string::npos)) {
                size_t bracePos = trimmed.find('{');
                std::string sig = (bracePos != std::string::npos) ? trim(trimmed.substr(0, bracePos)) : trimmed;
                std::string typeTag = (trimmed.find("struct") != std::string::npos) ? "struct" : "interface";
                entries.push_back({(long)(i+1), typeTag, sig}); continue;
            }
        }
        if (isCpp || isJava || !isPython) {
            if (trimmed.find("struct ") != std::string::npos && trimmed.find("struct ") < 20) {
                size_t bracePos = trimmed.find('{');
                size_t parenPos = trimmed.find('(');
                size_t endPos = std::min(bracePos, parenPos);
                std::string sig = (endPos != std::string::npos) ? trim(trimmed.substr(0, endPos)) : trimmed;
                if (sig.size() > 5) entries.push_back({(long)(i+1), "struct", sig});
                continue;
            }
            if (trimmed.find("class ") != std::string::npos && trimmed.find("class ") < 20) {
                size_t bracePos = trimmed.find('{');
                size_t colonPos = trimmed.find(':');
                size_t endPos = std::min(bracePos, colonPos);
                std::string sig = (endPos != std::string::npos) ? trim(trimmed.substr(0, endPos)) : trimmed;
                if (sig.size() > 5) entries.push_back({(long)(i+1), "class", sig});
                continue;
            }
            bool hasParen = trimmed.find('(') != std::string::npos && trimmed.find(')') != std::string::npos;
            bool endsWithBrace = (trimmed.back() == '{');
            if (!endsWithBrace && trimmed.find('{') != std::string::npos) {
                std::string tail = trim(trimmed.substr(trimmed.find('{')));
                if (tail == "{") endsWithBrace = true;
            }
            if (hasParen && endsWithBrace) {
                size_t bracePos = trimmed.find('{');
                std::string sig = trim(trimmed.substr(0, bracePos));
                if (sig.substr(0, 3) == "if " || sig.substr(0, 4) == "for " || sig.substr(0, 6) == "while " ||
                    sig.substr(0, 6) == "switch" || sig.substr(0, 4) == "else" || sig.substr(0, 5) == "catch" ||
                    sig.substr(0, 5) == "guard") continue;
                size_t parenPos = sig.find('(');
                if (parenPos > 0) entries.push_back({(long)(i+1), "func", sig});
                continue;
            }
            if (hasParen && trimmed.back() == ')') {
                if (i + 1 < lines.size() && (trim(lines[i+1]) == "{" || trim(lines[i+1]).substr(0, 1) == "{")) {
                    if (trimmed.substr(0, 3) == "if " || trimmed.substr(0, 4) == "for " ||
                        trimmed.substr(0, 6) == "while " || trimmed.substr(0, 6) == "switch" ||
                        trimmed.substr(0, 4) == "else" || trimmed.substr(0, 5) == "catch") continue;
                    size_t parenPos = trimmed.find('(');
                    if (parenPos > 0) entries.push_back({(long)(i+1), "func", trimmed});
                }
            }
        }
    }
    return entries;
}
Json toolOutline(const Json& args, const fs::path& baseDir) {
    std::vector<std::string> fileList;
    const std::vector<Json>* filesArr = args.getArr2("f", "files");
    if (!filesArr) filesArr = args.getArr2("files", "paths");
    if (!filesArr) filesArr = args.getArr2("p", "paths");
    if (filesArr) {
        for (auto& item : *filesArr) if (item.type == Json::Type::String) fileList.push_back(item.str);
    }
    std::string singleFile = args.getStr2("f", "file", "");
    if (singleFile.empty()) singleFile = args.getStr2("p", "path", "");
    if (!singleFile.empty()) fileList.push_back(singleFile);

    if (fileList.empty()) {
        Json r = Json::Obj(); r.set("err", Json::Str("'f' (file or files array) is required")); return r;
    }

    if (fileList.size() == 1) {
        std::string relFile = fileList[0];
        fs::path full;
        if (!safeResolve(baseDir, relFile, full)) {
            Json r = Json::Obj(); r.set("err", Json::Str("path escapes project sandbox: " + relFile)); return r;
        }
        std::error_code ec;
        if (!fs::exists(full, ec)) {
            Json r = Json::Obj(); r.set("err", Json::Str("not found: " + relFile)); return r;
        }
        std::vector<std::string> lines = readFileLines(full.string());
        std::string ext = toLower(full.extension().string());
        auto entries = extractOutline(lines, ext);
        std::ostringstream out;
        for (auto& e : entries) out << e.line << ":" << e.type << ": " << e.signature << "\n";
        Json r = Json::Obj();
        r.set("f", Json::Str(relFile));
        r.set("n", Json::Num((double)entries.size()));
        r.set("o", Json::Str(out.str()));
        return r;
    } else {
        Json outArr = Json::Arr();
        for (auto& relFile : fileList) {
            fs::path full;
            Json item = Json::Obj();
            item.set("f", Json::Str(relFile));
            if (!safeResolve(baseDir, relFile, full)) {
                item.set("err", Json::Str("path escapes project sandbox"));
            } else {
                std::error_code ec;
                if (!fs::exists(full, ec)) {
                    item.set("err", Json::Str("not found"));
                } else {
                    std::vector<std::string> lines = readFileLines(full.string());
                    std::string ext = toLower(full.extension().string());
                    auto entries = extractOutline(lines, ext);
                    std::ostringstream out;
                    for (auto& e : entries) out << e.line << ":" << e.type << ": " << e.signature << "\n";
                    item.set("n", Json::Num((double)entries.size()));
                    item.set("o", Json::Str(out.str()));
                }
            }
            outArr.arr.push_back(item);
        }
        Json r = Json::Obj();
        r.set("outlines", outArr);
        return r;
    }
}
Json toolRecent(const Json& args, const fs::path& baseDir) {
    long minutes = args.getInt2("min", "minutes", 30);
    long maxCount = args.getInt2("max", "max_files", 50);
    if (minutes < 1) minutes = 30;
    struct FileInfo { std::string relPath; std::filesystem::file_time_type mtime; uintmax_t size; };
    std::vector<FileInfo> recentFiles;
    std::error_code ec;
    std::vector<fs::path> files; walkDir(baseDir, files, true);
    for (auto& fp : files) {
        std::error_code sec;
        auto mtime = fs::last_write_time(fp, sec);
        if (sec) continue;
        auto fileNow = fs::file_time_type::clock::now();
        auto age = fileNow - mtime;
        if (age < std::chrono::minutes(0)) age = std::chrono::minutes(0);
        if (age > std::chrono::minutes(minutes)) continue;
        auto sz = fs::file_size(fp, sec);
        std::string relName = fs::relative(fp, baseDir, ec).string();
        if (ec) relName = fp.string();
        recentFiles.push_back({relName, mtime, sz});
    }
    std::sort(recentFiles.begin(), recentFiles.end(), [](const FileInfo& a, const FileInfo& b) {
        return a.mtime > b.mtime;
    });
    std::ostringstream out; long count = 0;
    for (auto& fi : recentFiles) {
        if (count >= maxCount) break;
        out << fi.relPath << " (" << humanSize(fi.size) << ((isBinaryExt(fs::path(fi.relPath)) || looksBinaryFile(baseDir / fi.relPath)) ? ", bin" : "") << ")\n"; count++;
    }
    Json r = Json::Obj();
    r.set("min", Json::Num((double)minutes));
    r.set("n", Json::Num((double)count));
    if (count >= maxCount) r.set("trunc", Json::Bool(true));
    r.set("files", Json::Str(out.str()));
    return r;
}

// ===========================================================================
// Tool: undo_tool  (alias: undo) — restore a file from its .mpt_backups history
// Every patch/edit that actually changes a file snapshots the pre-change
// content to baseDir/.mpt_backups/<safename>.<epoch_ns>.bak before writing.
// This tool lists or restores those snapshots, newest first.
// ===========================================================================
struct BackupInfo { fs::path path; long long ts; uintmax_t size; };

static void collectBackups(const fs::path& root, const std::string& relFile, std::vector<BackupInfo>& out) {
    fs::path bdir = root / ".mpt_backups";
    std::error_code ec;
    if (!fs::exists(bdir, ec) || !fs::is_directory(bdir, ec)) return;
    std::string prefix = backupSafeName(relFile) + ".";
    for (auto& entry : fs::directory_iterator(bdir, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code rec;
        if (!entry.is_regular_file(rec) || rec) continue;
        std::string fname = entry.path().filename().string();
        if (fname.size() <= prefix.size() + 4) continue;
        if (fname.compare(0, prefix.size(), prefix) != 0) continue;
        if (fname.substr(fname.size() - 4) != ".bak") continue;
        std::string tsStr = fname.substr(prefix.size(), fname.size() - prefix.size() - 4);
        long long ts = 0;
        try { ts = std::stoll(tsStr); } catch (...) { continue; }
        std::error_code sec;
        auto sz = fs::file_size(entry.path(), sec);
        out.push_back({entry.path(), ts, sec ? 0 : sz});
    }
}

std::vector<BackupInfo> listBackupsFor(const fs::path& baseDir, const std::string& relFile) {
    std::vector<BackupInfo> out;
    fs::path owner = rootFor(baseDir, relFile);
    collectBackups(owner, relFile, out);
    // A file deleted after its last backup resolves to no root — sweep the
    // other roots' history before reporting "no backups".
    if (out.empty()) {
        for (auto& r : g_roots) {
            if (r == owner) continue;
            collectBackups(r, relFile, out);
            if (!out.empty()) break;
        }
    }
    std::sort(out.begin(), out.end(), [](const BackupInfo& a, const BackupInfo& b) { return a.ts > b.ts; });
    return out;
}

Json toolUndo(const Json& args, const fs::path& baseDir) {
    std::string relFile = args.getStr2("f", "file");
    if (relFile.empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("'f' (file) is required"));
        return r;
    }
    auto backups = listBackupsFor(baseDir, relFile);
    if (args.getBool2("list", "list", false)) {
        Json arr = Json::Arr();
        for (size_t i = 0; i < backups.size(); ++i) {
            Json b = Json::Obj();
            b.set("n", Json::Num((double)(i + 1)));      // pass this as 'n' to restore it
            b.set("size", Json::Num((double)backups[i].size));
            arr.arr.push_back(b);
        }
        Json r = Json::Obj();
        r.set("f", Json::Str(relFile));
        r.set("n_available", Json::Num((double)backups.size()));
        r.set("backups", arr);
        r.set("note", Json::Str("newest first; n=1 is the state right before the most recent write"));
        return r;
    }
    if (backups.empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("no backups found for " + relFile +
                 " (a backup is only made the first time patch/edit changes a file)"));
        return r;
    }
    long n = args.getInt2("n", "n", 1);
    if (n < 1 || (size_t)n > backups.size()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("n out of range; " + std::to_string(backups.size()) + " backup(s) available"));
        return r;
    }
    fs::path full;
    if (!safeResolve(baseDir, relFile, full)) {
        Json r = Json::Obj();
        r.set("err", Json::Str("path escapes project sandbox: " + relFile));
        return r;
    }
    std::error_code ec;
    std::string currentContent = fs::exists(full, ec) ? readFileAll(full.string()) : "";
    std::string restoredContent = readFileAll(backups[(size_t)n - 1].path.string());
    // Snapshot current (pre-undo) state too, so undo is itself undoable: calling
    // undo again with n=1 restores what we're about to overwrite right now.
    backupFile(baseDir, relFile, currentContent);
    fs::create_directories(full.parent_path(), ec);
    bool ok = writeFileAll(full.string(), restoredContent);
    Json r = Json::Obj();
    r.set("f", Json::Str(relFile));
    r.set("st", Json::Str(ok ? "ok" : "fail"));
    if (!ok) {
        r.set("r", Json::Str("write failed"));
    } else {
        r.set("restored_backup", Json::Num((double)n));
        r.set("chk", Json::Str(fnv1a64Hex(restoredContent)));
        r.set("lines", Json::Num((double)splitLines(normalizeLineEndings(restoredContent)).size()));
        r.set("bytes", Json::Num((double)restoredContent.size()));
        r.set("note", Json::Str("pre-undo state was also snapshotted; call undo again with n=1 to redo"));
    }
    return r;
}

// ===========================================================================
// Tool: create_file_tool (alias: create_file) — dedicated file-creation tool
// Same underlying write path as edit's "create" mode (sandboxed, backs up
// before an overwrite, returns checksum/lines/bytes), exposed as its own
// tool for callers that want a direct, simple "make this file" verb rather
// than going through edit's array-of-modes shape.
// ===========================================================================
Json toolCreateFile(const Json& args, const fs::path& baseDir) {
    std::vector<Json> singleWrap;
    const std::vector<Json>* files = args.getArr2("files", "files");
    if (!files) {
        // Convenience: {"t":"create_file","a":{"f":"path","c":"content"}} directly,
        // no need to wrap a single file in a "files" array.
        if (!args.getStr2("f", "file").empty()) {
            singleWrap.push_back(args);
            files = &singleWrap;
        }
    }
    if (!files || files->empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("provide 'f' (+'c'/'cb64') directly, or a 'files' array of {f,c}"));
        return r;
    }
    Json results = Json::Arr();
    for (auto& fe : *files) {
        std::string relFile = fe.getStr2("f", "file");
        Json entry = Json::Obj();
        entry.set("f", Json::Str(relFile));

        try { // per-file guard: one bad entry must not lose the whole batch's results

        if (relFile.empty()) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("'f' (file) required"));
            results.arr.push_back(entry);
            continue;
        }
        fs::path full;
        if (!safeResolve(baseDir, relFile, full)) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("path escapes project sandbox: " + relFile));
            results.arr.push_back(entry);
            continue;
        }
        if (isProtectedPath(baseDir, full)) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("refusing to write to the project root or .mpt_backups"));
            results.arr.push_back(entry);
            continue;
        }
        std::string content = getTextOrB64(fe, "c", "content", "cb64");
        bool overwrite = fe.getBool2("ow", "overwrite", false);
        std::error_code ec;
        bool existed = fs::exists(full, ec);
        if (existed && fs::is_directory(full, ec)) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("a directory already exists at this path"));
            results.arr.push_back(entry);
            continue;
        }
        if (existed && !overwrite) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("exists (ow:true to replace)"));
            results.arr.push_back(entry);
            continue;
        }
        if (existed && overwrite) {
            backupFile(baseDir, relFile, readFileAll(full.string()));
        }
        fs::create_directories(full.parent_path(), ec);
        bool ok = writeFileAll(full.string(), content);
        entry.set("st", Json::Str(ok ? "ok" : "fail"));
        if (!ok) {
            entry.set("r", Json::Str("write failed"));
        } else {
            entry.set("overwrote", Json::Bool(existed));
            entry.set("chk", Json::Str(fnv1a64Hex(content)));
            entry.set("lines", Json::Num((double)countLines(content)));
            entry.set("bytes", Json::Num((double)content.size()));
        }
        results.arr.push_back(entry);

        } catch (std::exception& ex) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str(std::string("exception: ") + ex.what()));
            results.arr.push_back(entry);
        } catch (...) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("unknown exception processing this file"));
            results.arr.push_back(entry);
        }
    }
    Json out = Json::Obj();
    out.set("r", results);
    return out;
}

// ===========================================================================
// Tool: create_directory_tool (alias: create_directory, mkdir) — make dirs
// Creates one or more directories (with parents, like mkdir -p). Idempotent:
// an already-existing directory is reported ok, not an error. A file already
// occupying the path IS an error (won't silently do nothing or clobber it).
// ===========================================================================
Json toolCreateDirectory(const Json& args, const fs::path& baseDir) {
    std::vector<std::string> dirs;
    std::string single = args.getStr2("p", "path", "");
    if (!single.empty()) dirs.push_back(single);
    const std::vector<Json>* arr = args.getArr2("paths", "paths");
    if (arr) for (auto& d : *arr) if (d.type == Json::Type::String && !d.str.empty()) dirs.push_back(d.str);
    if (dirs.empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("provide 'p' (single path) and/or 'paths' (array of paths)"));
        return r;
    }
    Json results = Json::Arr();
    for (auto& d : dirs) {
        Json entry = Json::Obj();
        entry.set("p", Json::Str(d));

        try { // per-dir guard: one bad entry must not lose the whole batch's results

        fs::path full;
        if (!safeResolve(baseDir, d, full)) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("path escapes project sandbox: " + d));
            results.arr.push_back(entry);
            continue;
        }
        std::error_code ec;
        bool existedAsDir = fs::is_directory(full, ec);
        bool existedAsFile = !existedAsDir && fs::exists(full, ec);
        if (existedAsFile) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("a file already exists at this path"));
            results.arr.push_back(entry);
            continue;
        }
        if (existedAsDir) {
            entry.set("st", Json::Str("ok"));
            entry.set("existed", Json::Bool(true));
            results.arr.push_back(entry);
            continue;
        }
        bool created = fs::create_directories(full, ec);
        entry.set("st", Json::Str((created || !ec) ? "ok" : "fail"));
        if (ec) entry.set("r", Json::Str(ec.message()));
        entry.set("existed", Json::Bool(false));
        results.arr.push_back(entry);

        } catch (std::exception& ex) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str(std::string("exception: ") + ex.what()));
            results.arr.push_back(entry);
        } catch (...) {
            entry.set("st", Json::Str("fail"));
            entry.set("r", Json::Str("unknown exception processing this directory"));
            results.arr.push_back(entry);
        }
    }
    Json out = Json::Obj();
    out.set("r", results);
    return out;
}

// ===========================================================================
// Tool: cut_tool (alias: cut) — move (or copy) a line range out of one file
// and into another. For splitting a monolithic file into pieces while
// keeping content byte-identical: no re-typing, no diffing required.
// ===========================================================================
Json toolCut(const Json& args, const fs::path& baseDir) {
    std::string relFile = args.getStr2("f", "file");
    std::string relTo = args.getStr2("to", "to_file");
    long startLine = args.getInt2("s", "start", 0);
    long endLine = args.getInt2("e", "end", 0);
    std::string mode = args.getStr2("mode", "mode", "append"); // append|prepend|insert_after|create
    long atLine = args.getInt2("l", "line", -1);
    bool overwrite = args.getBool2("ow", "overwrite", false);
    bool keep = args.getBool2("keep", "keep", false); // true: copy instead of removing from source

    Json r = Json::Obj();
    if (relFile.empty()) { r.set("err", Json::Str("'f' (file) is required")); return r; }
    if (relTo.empty()) { r.set("err", Json::Str("'to' (destination file) is required")); return r; }
    if (startLine < 1) { r.set("err", Json::Str("'s' (start) must be >= 1")); return r; }
    if (endLine < startLine) endLine = startLine;

    fs::path fullSrc, fullDst;
    if (!safeResolve(baseDir, relFile, fullSrc)) { r.set("err", Json::Str("path escapes project sandbox: " + relFile)); return r; }
    if (!safeResolve(baseDir, relTo, fullDst)) { r.set("err", Json::Str("path escapes project sandbox: " + relTo)); return r; }
    if (isProtectedPath(baseDir, fullSrc) || isProtectedPath(baseDir, fullDst)) {
        r.set("err", Json::Str("refusing to read/write the project root or .mpt_backups"));
        return r;
    }
    std::error_code ec;
    if (!fs::exists(fullSrc, ec)) { r.set("err", Json::Str("source not found: " + relFile)); return r; }
    if (fs::is_directory(fullSrc, ec)) { r.set("err", Json::Str("source is a directory, not a file: " + relFile)); return r; }
    if (fs::exists(fullDst, ec) && fs::is_directory(fullDst, ec)) { r.set("err", Json::Str("destination is a directory, not a file: " + relTo)); return r; }

    std::string srcOriginal = readFileAll(fullSrc.string());
    std::vector<std::string> srcLines = splitLines(normalizeLineEndings(srcOriginal));
    if (endLine > (long)srcLines.size()) { r.set("err", Json::Str("range exceeds file length (" + std::to_string(srcLines.size()) + " lines)")); return r; }

    std::vector<std::string> cutLines(srcLines.begin() + (startLine - 1), srcLines.begin() + endLine);
    std::string cutContent = joinLines(cutLines);

    // Same-file cut/extract ("move this block to a different spot in the same
    // file", e.g. reordering functions while splitting a monolith) is handled
    // as one atomic reorder instead of two separate writes to the same path.
    // Doing it as two writes -- once to build "dst" content, once to build the
    // range-removed "src" content -- has the second write silently clobber the
    // first, deleting the cut block instead of relocating it.
    bool sameFile = false;
    {
        std::error_code cec;
        fs::path canonSrc = fs::weakly_canonical(fullSrc, cec);
        fs::path canonDst = fs::weakly_canonical(fullDst, cec);
        sameFile = !cec && canonSrc == canonDst;
        if (cec) sameFile = (fullSrc.lexically_normal() == fullDst.lexically_normal());
    }
    if (sameFile) {
        if (mode == "create") {
            r.set("err", Json::Str("mode:create is invalid when 'to' is the same file as 'f'; use append/prepend/insert_after to reorder within the file"));
            return r;
        }
        if (mode == "insert_after") {
            if (atLine < 0) { r.set("err", Json::Str("'l' (line) required for mode:insert_after")); return r; }
            if (atLine >= startLine && atLine <= endLine) {
                r.set("err", Json::Str("'l' (line " + std::to_string(atLine) + ") falls inside the range being moved (" +
                                       std::to_string(startLine) + "-" + std::to_string(endLine) + "); choose a target line outside it"));
                return r;
            }
            if (atLine > (long)srcLines.size()) { r.set("err", Json::Str("'l' (line) exceeds file length (" + std::to_string(srcLines.size()) + " lines)")); return r; }
        }
        long shift = endLine - startLine + 1;
        std::vector<std::string> finalLines = srcLines;
        long insertPos;
        if (!keep) finalLines.erase(finalLines.begin() + (startLine - 1), finalLines.begin() + endLine);
        if (mode == "prepend") insertPos = 0;
        else if (mode == "insert_after") insertPos = keep ? atLine : (atLine < startLine ? atLine : atLine - shift);
        else insertPos = (long)finalLines.size(); // append (default)
        if (insertPos < 0) insertPos = 0;
        if (insertPos > (long)finalLines.size()) insertPos = (long)finalLines.size();
        finalLines.insert(finalLines.begin() + insertPos, cutLines.begin(), cutLines.end());
        std::string finalContent = joinLines(finalLines);
        backupFile(baseDir, relFile, srcOriginal);
        if (!writeFileAll(fullSrc.string(), finalContent)) { r.set("err", Json::Str("write failed")); return r; }
        Json entry = Json::Obj();
        entry.set("f", Json::Str(relFile));
        entry.set("chk", Json::Str(fnv1a64Hex(finalContent)));
        entry.set("lines", Json::Num((double)finalLines.size()));
        entry.set("bytes", Json::Num((double)finalContent.size()));
        r.set("dst", entry);
        r.set("src", entry);
        r.set("same_file", Json::Bool(true));
        r.set("cut_lines", Json::Num((double)cutLines.size()));
        return r;
    }

    // -- write/update destination --
    bool dstExisted = fs::exists(fullDst, ec);
    std::string dstOriginal = dstExisted ? readFileAll(fullDst.string()) : "";
    std::string finalDstContent;
    if (mode == "create") {
        if (dstExisted && !overwrite) { r.set("err", Json::Str("destination exists (ow:true to replace, or use mode:append/prepend/insert_after)")); return r; }
        finalDstContent = cutContent;
    } else if (!dstExisted) {
        finalDstContent = cutContent; // destination doesn't exist yet: any mode just creates it
    } else {
        std::vector<std::string> dstLines = splitLines(normalizeLineEndings(dstOriginal));
        if (mode == "prepend") {
            dstLines.insert(dstLines.begin(), cutLines.begin(), cutLines.end());
        } else if (mode == "insert_after") {
            if (atLine < 0 || atLine > (long)dstLines.size()) { r.set("err", Json::Str("'l' (line) required and must be within destination (0.." + std::to_string(dstLines.size()) + ") for mode:insert_after")); return r; }
            dstLines.insert(dstLines.begin() + atLine, cutLines.begin(), cutLines.end());
        } else { // append (default)
            dstLines.insert(dstLines.end(), cutLines.begin(), cutLines.end());
        }
        finalDstContent = joinLines(dstLines);
    }

    if (dstExisted) backupFile(baseDir, relTo, dstOriginal);
    fs::create_directories(fullDst.parent_path(), ec);
    if (!writeFileAll(fullDst.string(), finalDstContent)) { r.set("err", Json::Str("write to destination failed")); return r; }

    Json dstEntry = Json::Obj();
    dstEntry.set("f", Json::Str(relTo));
    dstEntry.set("chk", Json::Str(fnv1a64Hex(finalDstContent)));
    dstEntry.set("lines", Json::Num((double)splitLines(finalDstContent).size()));
    dstEntry.set("bytes", Json::Num((double)finalDstContent.size()));
    r.set("dst", dstEntry);

    // -- remove from source unless keep:true --
    Json srcEntry = Json::Obj();
    srcEntry.set("f", Json::Str(relFile));
    if (!keep) {
        srcLines.erase(srcLines.begin() + (startLine - 1), srcLines.begin() + endLine);
        std::string finalSrcContent = joinLines(srcLines);
        backupFile(baseDir, relFile, srcOriginal);
        if (!writeFileAll(fullSrc.string(), finalSrcContent)) { r.set("err", Json::Str("cut destination written, but source rewrite failed (source unchanged)")); return r; }
        srcEntry.set("chk", Json::Str(fnv1a64Hex(finalSrcContent)));
        srcEntry.set("lines", Json::Num((double)srcLines.size()));
        srcEntry.set("bytes", Json::Num((double)finalSrcContent.size()));
    } else {
        srcEntry.set("unchanged", Json::Bool(true));
    }
    r.set("src", srcEntry);
    r.set("cut_lines", Json::Num((double)cutLines.size()));
    return r;
}

// ===========================================================================
// Tool: extract_tool (alias: extract) — find a function/class/struct by name
// (via the same detector outline_tool uses) and copy or move its full body
// into another file, without the caller having to compute line ranges by
// hand. Falls back to an explicit 's'/'e' range when 'name' isn't given or
// auto-detection isn't supported for the file's language.
// ===========================================================================
// Strips string literals, char literals, and // and /* */ comments from a
// line of C-like source before brace counting, so a brace inside a string or
// comment (e.g. `printf("if (x) { }\n");` or `// mismatched {`) doesn't desync
// the depth counter. inBlockComment carries across calls for a /* ... */ that
// spans multiple lines.
std::string stripStringsAndComments(const std::string& line, bool& inBlockComment) {
    std::string out;
    out.reserve(line.size());
    size_t i = 0;
    while (i < line.size()) {
        if (inBlockComment) {
            size_t end = line.find("*/", i);
            if (end == std::string::npos) break;
            inBlockComment = false;
            i = end + 2;
            continue;
        }
        char c = line[i];
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break; // rest of line is a line comment
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') { inBlockComment = true; i += 2; continue; }
        if (c == '"' || c == '\'') {
            char quote = c;
            i++;
            while (i < line.size() && line[i] != quote) {
                if (line[i] == '\\' && i + 1 < line.size()) i++; // skip escaped char
                i++;
            }
            if (i < line.size()) i++; // skip closing quote
            continue;
        }
        out.push_back(c);
        i++;
    }
    return out;
}
bool findBodyRangeByBraces(const std::vector<std::string>& lines, long startLine, long& endLine) {
    long depth = 0;
    bool seenOpen = false;
    bool inBlockComment = false;
    // Sanity cap: give up instead of sweeping through unrelated trailing code
    // if the located "symbol" line turns out to be a bodyless prototype (no
    // '{' ever found) or the depth just never closes.
    const long maxScan = 5000;
    for (size_t i = (size_t)(startLine - 1); i < lines.size() && (long)(i - (size_t)(startLine - 1)) < maxScan; ++i) {
        std::string stripped = stripStringsAndComments(lines[i], inBlockComment);
        for (char c : stripped) {
            if (c == '{') { depth++; seenOpen = true; }
            else if (c == '}') { depth--; }
        }
        if (seenOpen && depth <= 0) { endLine = (long)(i + 1); return true; }
    }
    return false;
}
bool findBodyRangeByIndent(const std::vector<std::string>& lines, long startLine, long& endLine) {
    if (startLine < 1 || startLine > (long)lines.size()) return false;
    std::string first = lines[startLine - 1];
    size_t baseIndent = first.find_first_not_of(" \t");
    if (baseIndent == std::string::npos) baseIndent = 0;
    long last = startLine;
    for (size_t i = (size_t)startLine; i < lines.size(); ++i) {
        std::string t = trim(lines[i]);
        if (t.empty()) { continue; } // blank lines don't end the block by themselves
        size_t ind = lines[i].find_first_not_of(" \t");
        if (ind != std::string::npos && ind <= baseIndent) break;
        last = (long)(i + 1);
    }
    endLine = last;
    return true;
}
// True if `name` appears in `text` as a standalone identifier token, not as a
// substring of a longer one. Prevents name:"run" from matching "runAll",
// "tryRun", or "runner".
bool containsIdentifier(const std::string& text, const std::string& name) {
    if (name.empty()) return false;
    size_t pos = 0;
    while ((pos = text.find(name, pos)) != std::string::npos) {
        bool leftOk = (pos == 0) || !(isalnum((unsigned char)text[pos - 1]) || text[pos - 1] == '_');
        size_t endPos = pos + name.size();
        bool rightOk = (endPos >= text.size()) || !(isalnum((unsigned char)text[endPos]) || text[endPos] == '_');
        if (leftOk && rightOk) return true;
        pos += 1;
    }
    return false;
}
Json toolExtract(const Json& args, const fs::path& baseDir) {
    std::string relFile = args.getStr2("f", "file");
    std::string relTo = args.getStr2("to", "to_file");
    std::string name = args.getStr2("name", "name");
    long startLine = args.getInt2("s", "start", 0);
    long endLine = args.getInt2("e", "end", 0);
    std::string mode = args.getStr2("mode", "mode", "copy"); // copy|move
    long occ = args.getInt2("occ", "occurrence", 0);

    Json r = Json::Obj();
    if (relFile.empty()) { r.set("err", Json::Str("'f' (file) is required")); return r; }
    if (relTo.empty()) { r.set("err", Json::Str("'to' (destination file) is required")); return r; }
    fs::path fullSrc;
    if (!safeResolve(baseDir, relFile, fullSrc)) { r.set("err", Json::Str("path escapes project sandbox: " + relFile)); return r; }
    std::error_code ec;
    if (!fs::exists(fullSrc, ec)) { r.set("err", Json::Str("source not found: " + relFile)); return r; }
    if (fs::is_directory(fullSrc, ec)) { r.set("err", Json::Str("source is a directory, not a file: " + relFile)); return r; }
    std::vector<std::string> lines = readFileLines(fullSrc.string());

    if (startLine < 1) {
        if (name.empty()) { r.set("err", Json::Str("provide either 'name' (symbol to locate) or explicit 's'/'e' line range")); return r; }
        std::string ext = toLower(fullSrc.extension().string());
        auto entries = extractOutline(lines, ext);
        std::vector<long> matchLines;
        std::vector<std::string> matchSigs;
        for (auto& en : entries) {
            if (containsIdentifier(en.signature, name)) {
                matchLines.push_back(en.line);
                matchSigs.push_back(en.signature);
            }
        }
        if (matchLines.empty()) { r.set("err", Json::Str("no symbol matching identifier '" + name + "' found in outline of " + relFile)); return r; }
        long foundLine;
        if (matchLines.size() > 1 && occ == 0) {
            Json matchInfo = Json::Arr();
            for (size_t i = 0; i < matchLines.size(); ++i) {
                Json m = Json::Obj();
                m.set("line", Json::Num((double)matchLines[i]));
                std::string sig = matchSigs[i];
                if (sig.size() > 80) sig = sig.substr(0, 80);
                m.set("sig", Json::Str(sig));
                matchInfo.arr.push_back(m);
            }
            r.set("err", Json::Str("ambiguous: " + std::to_string(matchLines.size()) +
                                   " symbols match identifier '" + name + "'; pass 'occ' (1-based)"));
            r.set("matches", matchInfo);
            return r;
        }
        if (occ != 0) {
            if (occ < 1 || (size_t)occ > matchLines.size()) {
                r.set("err", Json::Str("occ out of range; " + std::to_string(matchLines.size()) + " matches found"));
                return r;
            }
            foundLine = matchLines[(size_t)occ - 1];
        } else {
            foundLine = matchLines[0];
        }
        startLine = foundLine;
        bool isPython = (ext == ".py" || ext == ".pyw" || ext == ".pyi");
        long detectedEnd = 0;
        bool ok = isPython ? findBodyRangeByIndent(lines, startLine, detectedEnd)
                            : findBodyRangeByBraces(lines, startLine, detectedEnd);
        if (!ok) { r.set("err", Json::Str("found '" + name + "' at line " + std::to_string(startLine) + " but could not auto-detect its end; pass explicit 's'/'e' instead")); return r; }
        endLine = detectedEnd;
    }
    if (endLine < startLine) endLine = startLine;

    Json cutArgs = Json::Obj();
    cutArgs.set("f", Json::Str(relFile));
    cutArgs.set("to", Json::Str(relTo));
    cutArgs.set("s", Json::Num((double)startLine));
    cutArgs.set("e", Json::Num((double)endLine));
    cutArgs.set("mode", Json::Str(args.getStr2("dst_mode", "dst_mode", "append")));
    cutArgs.set("ow", Json::Bool(args.getBool2("ow", "overwrite", false)));
    if (args.get("l")) cutArgs.set("l", *args.get("l"));
    else if (args.get("line")) cutArgs.set("l", *args.get("line"));
    cutArgs.set("keep", Json::Bool(mode != "move"));
    Json out = toolCut(cutArgs, baseDir);
    if (name.empty()) return out;
    Json wrapped = Json::Obj();
    wrapped.set("name", Json::Str(name));
    wrapped.set("s", Json::Num((double)startLine));
    wrapped.set("e", Json::Num((double)endLine));
    for (auto& kv : out.obj) wrapped.set(kv.first, kv.second);
    return wrapped;
}

// ===========================================================================
// Tool: fileops_tool (alias: fileops) — whole-file management (copy, move,
// delete, stat) as a batch of ops, separate from content-level edit/patch.
// ===========================================================================
Json toolFileOps(const Json& args, const fs::path& baseDir) {
    std::vector<Json> singleWrap;
    const std::vector<Json>* ops = args.getArr2("ops", "ops");
    if (!ops) {
        if (!args.getStr2("op", "op").empty()) { singleWrap.push_back(args); ops = &singleWrap; }
    }
    if (!ops || ops->empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("provide 'op'(+'f'/'to') directly, or an 'ops' array of {op,f,to}"));
        return r;
    }
    Json results = Json::Arr();
    for (auto& oe : *ops) {
        std::string op = oe.getStr2("op", "op");
        std::string relFile = oe.getStr2("f", "file");
        std::string relTo = oe.getStr2("to", "to_file");
        bool overwrite = oe.getBool2("ow", "overwrite", false);
        bool recursive = oe.getBool2("r", "recursive", false);
        Json entry = Json::Obj();
        entry.set("op", Json::Str(op));
        entry.set("f", Json::Str(relFile));

        try { // per-op guard: one bad op must not lose the whole batch's results

        if (relFile.empty()) {
            entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("'f' (file) required"));
            results.arr.push_back(entry); continue;
        }
        fs::path full;
        if (!safeResolve(baseDir, relFile, full)) {
            entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("path escapes project sandbox: " + relFile));
            results.arr.push_back(entry); continue;
        }
        std::error_code ec;

        if (op == "stat") {
            if (!fs::exists(full, ec)) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("not found")); results.arr.push_back(entry); continue; }
            bool isDir = fs::is_directory(full, ec);
            entry.set("st", Json::Str("ok"));
            entry.set("is_dir", Json::Bool(isDir));
            if (!isDir) entry.set("bytes", Json::Num((double)fs::file_size(full, ec)));
            results.arr.push_back(entry); continue;
        }

        if (op == "delete" || op == "rm") {
            if (isProtectedPath(baseDir, full)) {
                entry.set("st", Json::Str("fail"));
                entry.set("r", Json::Str("refusing to delete the project root or .mpt_backups"));
                results.arr.push_back(entry); continue;
            }
            if (!fs::exists(full, ec)) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("not found")); results.arr.push_back(entry); continue; }
            bool isDir = fs::is_directory(full, ec);
            if (isDir && !recursive) {
                entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("is a directory (r:true to remove recursively)"));
                results.arr.push_back(entry); continue;
            }
            if (!isDir) backupFile(baseDir, relFile, readFileAll(full.string()));
            uintmax_t n = isDir ? fs::remove_all(full, ec) : (fs::remove(full, ec) ? 1 : 0);
            entry.set("st", Json::Str(!ec ? "ok" : "fail"));
            if (ec) entry.set("r", Json::Str(ec.message()));
            else entry.set("removed", Json::Num((double)n));
            results.arr.push_back(entry); continue;
        }

        if (op == "copy" || op == "cp" || op == "move" || op == "mv" || op == "rename") {
            if (relTo.empty()) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("'to' (destination) required")); results.arr.push_back(entry); continue; }
            fs::path fullTo;
            if (!safeResolve(baseDir, relTo, fullTo)) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("destination escapes project sandbox: " + relTo)); results.arr.push_back(entry); continue; }
            bool isMove = (op != "copy" && op != "cp");
            if (isMove && isProtectedPath(baseDir, full)) {
                entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("refusing to move/rename the project root or .mpt_backups"));
                results.arr.push_back(entry); continue;
            }
            if (isProtectedPath(baseDir, fullTo)) {
                entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("refusing to write over the project root or .mpt_backups"));
                results.arr.push_back(entry); continue;
            }
            if (!fs::exists(full, ec)) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("source not found")); results.arr.push_back(entry); continue; }
            bool toExisted = fs::exists(fullTo, ec);
            if (toExisted && !overwrite) { entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("destination exists (ow:true to replace)")); results.arr.push_back(entry); continue; }
            if (toExisted && !fs::is_directory(fullTo, ec)) backupFile(baseDir, relTo, readFileAll(fullTo.string()));
            fs::create_directories(fullTo.parent_path(), ec);
            entry.set("to", Json::Str(relTo));
            if (op == "copy" || op == "cp") {
                auto copyOpts = fs::copy_options::recursive;
                if (overwrite) copyOpts |= fs::copy_options::overwrite_existing;
                fs::copy(full, fullTo, copyOpts, ec);
            } else {
                fs::rename(full, fullTo, ec);
                if (ec) { // cross-device fallback
                    ec.clear();
                    fs::copy(full, fullTo, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                    if (!ec) fs::remove_all(full, ec);
                }
            }
            entry.set("st", Json::Str(!ec ? "ok" : "fail"));
            if (ec) entry.set("r", Json::Str(ec.message()));
            results.arr.push_back(entry); continue;
        }

        entry.set("st", Json::Str("fail"));
        entry.set("r", Json::Str("unknown op: '" + op + "' (use copy/move/delete/stat)"));
        results.arr.push_back(entry);

        } catch (std::exception& ex) {
            entry.set("st", Json::Str("fail")); entry.set("r", Json::Str(std::string("exception: ") + ex.what()));
            results.arr.push_back(entry);
        } catch (...) {
            entry.set("st", Json::Str("fail")); entry.set("r", Json::Str("unknown exception processing this op"));
            results.arr.push_back(entry);
        }
    }
    Json out = Json::Obj();
    out.set("r", results);
    return out;
}

// ===========================================================================
// Tool: diagnostics_tool (alias: diagnostics, diag) — run a build/lint/test
// command (same execution path as sh) and additionally parse GCC/Clang-style
// "file:line:col: severity: message" lines out of the output into a
// structured array, so callers don't have to regex the raw text themselves.
// ===========================================================================
Json toolDiagnostics(const Json& args, const fs::path& baseDir) {
    std::string cmd = args.getStr2("cmd", "command");
    if (cmd.empty()) { Json r = Json::Obj(); r.set("err", Json::Str("'cmd' is required")); return r; }
    long maxLen = args.getInt2("max", "max_output", 6000);
    long timeout = args.getInt2("to", "timeout", 60);
    if (timeout < 1) timeout = 60;
    if (timeout > 600) timeout = 600;
    fs::path cwd = baseDir;
    std::string dirArg = args.getStr2("dir", "cwd", "");
    if (!dirArg.empty()) {
        fs::path resolved;
        if (!safeResolve(baseDir, dirArg, resolved)) {
            Json r = Json::Obj();
            r.set("err", Json::Str("'dir' is outside all registered workspaces: " + dirArg));
            return r;
        }
        std::error_code dec;
        fs::path canon = fs::canonical(resolved, dec);
        if (dec || !fs::is_directory(canon, dec) || !isRegisteredRoot(baseDir, canon)) {
            Json r = Json::Obj();
            r.set("err", Json::Str("'dir' must be a registered workspace root, got: " + dirArg));
            return r;
        }
        cwd = canon;
    }
    auto [code, output] = runShellTimed(cmd, cwd, timeout);

    static const std::regex diagRe(R"(^([^:\n]+):(\d+):(?:(\d+):)?\s*(fatal error|error|warning|note)\s*:\s*(.*)$)",
                                    std::regex::icase);
    std::vector<std::string> outLines = splitLines(normalizeLineEndings(output));
    Json diags = Json::Arr();
    long errCount = 0, warnCount = 0;
    for (auto& ln : outLines) {
        std::smatch m;
        if (std::regex_match(ln, m, diagRe)) {
            Json d = Json::Obj();
            d.set("f", Json::Str(m[1].str()));
            d.set("l", Json::Num(std::stod(m[2].str())));
            if (m[3].matched && !m[3].str().empty()) d.set("c", Json::Num(std::stod(m[3].str())));
            std::string sev = toLower(m[4].str());
            d.set("sev", Json::Str(sev));
            d.set("msg", Json::Str(trim(m[5].str())));
            if (sev.find("error") != std::string::npos) errCount++;
            else if (sev == "warning") warnCount++;
            diags.arr.push_back(d);
        }
    }
    Json r = Json::Obj();
    r.set("rc", Json::Num(code));
    if (code == -1000) r.set("err", Json::Str("timeout"));
    else if (code == -1001) r.set("err", Json::Str("launch_failed"));
    r.set("errors", Json::Num((double)errCount));
    r.set("warnings", Json::Num((double)warnCount));
    r.set("diags", diags);
    r.set("out", Json::Str(truncateOutput(output, maxLen)));
    return r;
}

// ===========================================================================
// Tool: add_dir_tool  (alias: add_dir) — register an extra workspace root
// mid-session. Afterwards every tool accepts paths inside any registered
// root (the sandbox is the union of roots). Called with no 'p': lists them.
// ===========================================================================
Json toolAddDir(const Json& args, const fs::path& baseDir) {
    Json r = Json::Obj();
    std::string p = args.getStr2("p", "path", "");
    if (p.empty()) {
        std::vector<fs::path> roots = g_roots.empty() ? std::vector<fs::path>{baseDir} : g_roots;
        Json arr = Json::Arr();
        for (auto& root : roots) arr.arr.push_back(Json::Str(root.string()));
        r.set("workspaces", arr);
        r.set("n", Json::Num((double)roots.size()));
        r.set("note", Json::Str("relative paths: an existing file resolves to whichever workspace holds it; a new relative path is created in the primary workspace"));
        return r;
    }
    static const size_t MAX_ROOTS = 6;
    std::vector<fs::path> current = g_roots.empty() ? std::vector<fs::path>{baseDir} : g_roots;
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(p), ec);
    if (ec) { r.set("err", Json::Str("cannot interpret path: " + p)); return r; }
    fs::path canon = fs::canonical(abs, ec);
    if (ec || !fs::is_directory(canon, ec)) { r.set("err", Json::Str("not an existing directory: " + p)); return r; }
    if (canon == canon.root_path()) { r.set("err", Json::Str("refusing to register the filesystem root '/'")); return r; }
    for (auto& root : current) {
        if (root == canon) {
            r.set("st", Json::Str("already"));
            r.set("r", Json::Str("workspace already registered: " + canon.string()));
            return r;
        }
    }
    if (current.size() >= MAX_ROOTS) {
        r.set("err", Json::Str("workspace limit reached (" + std::to_string(MAX_ROOTS) + ")"));
        return r;
    }
    // Promote the implicit primary (baseDir) into the registry on first add,
    // so ordering stays: primary first, then mid-session additions.
    g_roots = current;
    g_roots.push_back(canon);
    r.set("st", Json::Str("ok"));
    r.set("added", Json::Str(canon.string()));
    r.set("n_workspaces", Json::Num((double)g_roots.size()));
    return r;
}

// ===========================================================================
// Dispatcher
// ===========================================================================
Json dispatchSingle(const Json& call, const fs::path& baseDir, std::string& toolName) {
    toolName = call.getStr2("t", "tool");
    if (toolName.empty()) {
        Json r = Json::Obj();
        r.set("err", Json::Str("missing 't' (tool) field"));
        return r;
    }
    const Json* argsPtr = call.get("a");
    if (!argsPtr) argsPtr = call.get("args");
    Json args = argsPtr ? *argsPtr : Json::Obj();
    Json result;
    auto t0 = std::chrono::steady_clock::now();
    try {
        if (toolName == "sh" || toolName == "shell_command_tool") result = toolShellCommand(args, baseDir);
        else if (toolName == "search" || toolName == "multi_search_tool") result = toolMultiSearch(args, baseDir);
        else if (toolName == "read" || toolName == "multi_read_tool") result = toolMultiRead(args, baseDir);
        else if (toolName == "patch" || toolName == "multi_patch_tool") result = toolMultiPatch(args, baseDir);
        else if (toolName == "edit" || toolName == "file_edit_tool") result = toolFileEdit(args, baseDir);
        else if (toolName == "git" || toolName == "git_tool") result = toolGit(args, baseDir);
        else if (toolName == "list" || toolName == "list_tool") result = toolList(args, baseDir);
        else if (toolName == "find" || toolName == "find_tool") result = toolFind(args, baseDir);
        else if (toolName == "outline" || toolName == "outline_tool") result = toolOutline(args, baseDir);
        else if (toolName == "recent" || toolName == "recent_tool") result = toolRecent(args, baseDir);
        else if (toolName == "undo" || toolName == "undo_tool") result = toolUndo(args, baseDir);
        else if (toolName == "create_file" || toolName == "create_file_tool") result = toolCreateFile(args, baseDir);
        else if (toolName == "create_directory" || toolName == "create_directory_tool" ||
                 toolName == "create_dir" || toolName == "mkdir") result = toolCreateDirectory(args, baseDir);
        else if (toolName == "cut" || toolName == "cut_tool") result = toolCut(args, baseDir);
        else if (toolName == "extract" || toolName == "extract_tool") result = toolExtract(args, baseDir);
        else if (toolName == "fileops" || toolName == "fileops_tool" || toolName == "fops") result = toolFileOps(args, baseDir);
        else if (toolName == "diagnostics" || toolName == "diagnostics_tool" || toolName == "diag") result = toolDiagnostics(args, baseDir);
        else if (toolName == "add_dir" || toolName == "add_dir_tool") result = toolAddDir(args, baseDir);
        else {
            result = Json::Obj();
            result.set("err", Json::Str("unknown tool: '" + toolName +
                "' (valid: sh, search, read, patch, edit, git, list, find, outline, recent, undo, "
                "create_file, create_directory, cut, extract, fileops, diagnostics, add_dir)"));
        }
    } catch (std::exception& e) {
        result = Json::Obj();
        result.set("err", Json::Str(std::string("exception in ") + toolName + ": " + e.what()));
    } catch (...) {
        result = Json::Obj();
        result.set("err", Json::Str("unknown exception in " + toolName));
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    result.set("t", Json::Str(toolName));
    result.set("ms", Json::Num((double)ms));
    return result;
}

// ===========================================================================
// Output: clearly highlighted, machine-parseable markers
// ===========================================================================
void printResult(const Json& j, const std::string& toolName) {
    g_statCalls++;
    g_lastTool = toolName;
    const Json* ms = j.get("ms");
    if (ms) g_lastMs = (long)ms->num;
    std::string dumped = j.dump();
    if (g_maxOutputChars > 0 && dumped.size() > g_maxOutputChars) {
        std::string esc;
        jsonEscape(dumped.substr(0, g_maxOutputChars), esc);
        std::ostringstream wrapped;
        wrapped << "{\"trunc\":true,\"orig_chars\":" << dumped.size()
                << ",\"cap\":" << g_maxOutputChars
                << ",\"note\":\"Output exceeded the configured paste-size cap and was cut off "
                   "(this JSON is a wrapper, not the tool's real shape). Ask for a narrower line "
                   "range (s/e), fewer files per call, or raise --max-output/TOOLS_MAX_OUTPUT.\","
                << "\"partial\":\"" << esc << "\"}";
        dumped = wrapped.str();
    }
    if (g_plainMode) {
        *g_out << dumped << "\n";
        g_out->flush();
        return;
    }
    *g_out << "\n" << BG_GRN << BOLD << " \u25bc TOOL RESULT " << RST << " " << GRN << "(" << toolName << ")" << RST << "\n";
    *g_out << GRN << "\u250c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" << RST << "\n";
    *g_out << GRN << "\u2502" << RST;
    *g_out << " " << dumped << "\n";
    *g_out << GRN << "\u2514\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500" << RST << "\n";
    g_out->flush();
}

void printError(const std::string& msg, const std::string& detail = "") {
    if (g_plainMode) {
        std::string esc; jsonEscape(msg, esc);
        std::string escD; jsonEscape(detail, escD);
        *g_out << "{\"err\":\"" << esc << "\"";
        if (!detail.empty()) *g_out << ",\"detail\":\"" << escD << "\"";
        *g_out << "}\n";
        g_out->flush();
        return;
    }
    *g_out << "\n" << BG_RED << BOLD << " \u2716 ERROR " << RST << " " << RED << msg << RST;
    if (!detail.empty()) *g_out << "\n" << RED << "  " << detail << RST;
    *g_out << "\n";
    g_out->flush();
}

void handleCommand(const std::string& text, const fs::path& baseDir) {
    std::string parseErr;
    Json cmd = Json::parse(text, &parseErr);
    if (!parseErr.empty()) {
        printError("JSON parse error", std::string(parseErr) + " | input was " +
                   std::to_string(text.size()) + " chars");
        if (text.size() > 200) {
            std::cout << RED << "  tail of input: ..." << text.substr(text.size() - 200) << RST << "\n";
        }
        std::cout.flush();
        return;
    }
    Json out;
    std::string lastTool = "batch";
    const std::vector<Json>* calls = nullptr;
    std::vector<Json> rootArrayCalls; // holds a copy when the root itself is the array
    if (cmd.type == Json::Type::Array) {
        // Bare top-level array of call objects, e.g. [ {"t":"read","a":{...}}, ... ]
        // Treated identically to {"calls":[...]} so it produces ONE combined result
        // instead of each element being missed/mishandled individually.
        rootArrayCalls = cmd.arr;
        calls = &rootArrayCalls;
    } else {
        calls = cmd.getArr2("calls", "calls");
    }
    if (calls) {
        Json results = Json::Arr();
        for (auto& c : *calls) {
            std::string tn;
            Json r = dispatchSingle(c, baseDir, tn);
            lastTool = tn;
            results.arr.push_back(r);
        }
        out = Json::Obj();
        out.set("r", results);
    } else {
        out = dispatchSingle(cmd, baseDir, lastTool);
    }
    printResult(out, lastTool);
}

// ===========================================================================
// Stdin JSON accumulator — robust with overflow + corruption detection
// ===========================================================================
struct JsonAccumulator {
    std::string buffer;
    int depth = 0;
    bool started = false;
    bool inString = false;
    bool escaped = false;
    bool complete = false;
    bool overflow = false;
    size_t startByte = 0;
    // Was a fixed 16MB constexpr; now tracks g_maxInputBytes so large writes
    // (e.g. pasting a big create_file payload) can be raised via --max-input=N
    // or TOOLS_MAX_INPUT without recompiling. See feed() below.
    size_t MAX_BUFFER = 0;

    void reset() {
        buffer.clear(); depth = 0; started = false; inString = false;
        escaped = false; complete = false; overflow = false; startByte = 0;
        MAX_BUFFER = g_maxInputBytes;
        buffer.reserve(std::min<size_t>(g_maxInputBytes, 4 * 1024 * 1024));
    }
    void feed(char c) {
        if (!started) {
            if (c == '{' || c == '[') { started = true; depth = 1; buffer.push_back(c); }
            return;
        }
        buffer.push_back(c);
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
        } else {
            if (c == '"') inString = true;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                depth--;
                if (depth == 0) { complete = true; return; }
                if (depth < 0) depth = 0;
            }
        }
        if (buffer.size() > MAX_BUFFER) overflow = true;
    }
};

// ===========================================================================
// Help text
// ===========================================================================
// ===========================================================================
// Embedded HTTP relay (nexon_relay) — lets the Android browser app drive
// this binary over 127.0.0.1. Contract: PROTOCOL.md. One command at a time:
// one background thread runs the accept loop and handles each connection
// synchronously, and handleCommandCapture holds g_outMutex, so a second
// in-flight request can never race the first. nexon_code is not
// concurrency-safe on one project root.
// ===========================================================================

static std::string jsonStr(const std::string& s) {
    std::string esc;
    jsonEscape(s, esc);
    return "\"" + esc + "\"";
}

// Run one command with output captured to a string instead of the terminal.
static std::string handleCommandCapture(const std::string& text, const fs::path& baseDir) {
    std::lock_guard<std::mutex> lk(g_outMutex);
    std::ostream* saved = g_out;
    bool savedPlain = g_plainMode;
    bool savedColorOut = g_colorOut;
    std::ostringstream oss;
    g_out = &oss;
    // Capture is machine-to-machine: the browser types this straight into a
    // chat composer, so force plain regardless of the interactive flag.
    // ANSI escapes and box-drawing glyphs would arrive there as garbage.
    g_plainMode = true;
    g_colorOut = false;
    try {
        handleCommand(text, baseDir);
    } catch (std::exception& e) {
        oss << "{\"err\":" << jsonStr(std::string("exception in handleCommand: ") + e.what()) << "}";
    } catch (...) {
        oss << "{\"err\":\"unknown exception in handleCommand\"}";
    }
    g_plainMode = savedPlain;
    g_colorOut = savedColorOut;
    g_out = saved;
    return oss.str();
}

static bool sendAllBytes(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        off += (size_t)n;
    }
    return true;
}

static void httpRespondRaw(int fd, int status, const std::string& body) {
    const char* reason =
        (status == 200) ? "OK" :
        (status == 400) ? "Bad Request" :
        (status == 404) ? "Not Found" :
        (status == 413) ? "Payload Too Large" :
        (status == 429) ? "Too Many Requests" : "Internal Server Error";
    std::ostringstream h;
    h << "HTTP/1.1 " << status << " " << reason << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "Cache-Control: no-store\r\n\r\n";
    sendAllBytes(fd, h.str() + body);
}

static std::string headerValue(const std::string& headers, const std::string& key) {
    std::string lower = toLower(headers);
    std::string kl = toLower(key) + ":";
    size_t p = lower.find(kl);
    if (p == std::string::npos) return "";
    p += kl.size();
    while (p < headers.size() && (headers[p] == ' ' || headers[p] == '\t')) p++;
    size_t e = headers.find("\r\n", p);
    if (e == std::string::npos) e = headers.size();
    return trim(headers.substr(p, e - p));
}

static void handleRelayConn(int fd, const fs::path& baseDir) {
    const size_t MAX_HEAD = 32 * 1024;
    const size_t MAX_BODY = 4 * 1024 * 1024;

    std::string buf;
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        if (buf.size() > MAX_HEAD) {
            httpRespondRaw(fd, 400, "{\"ok\":false,\"err\":\"headers too large\"}");
            return;
        }
        char tmp[2048];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) return;
        if (n < 0) { if (errno == EINTR) continue; return; }
        buf.append(tmp, (size_t)n);
        headerEnd = buf.find("\r\n\r\n");
    }
    std::string headers = buf.substr(0, headerEnd);
    std::string body = buf.substr(headerEnd + 4);

    size_t sp1 = headers.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : headers.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        httpRespondRaw(fd, 400, "{\"ok\":false,\"err\":\"bad request line\"}");
        return;
    }
    std::string method = headers.substr(0, sp1);
    std::string path = headers.substr(sp1 + 1, sp2 - sp1 - 1);

    size_t clen = 0;
    std::string cl = headerValue(headers, "content-length");
    if (!cl.empty()) { try { clen = (size_t)std::stoul(cl); } catch (...) {} }
    if (clen > MAX_BODY) {
        httpRespondRaw(fd, 413, "{\"ok\":false,\"err\":\"body too large\"}");
        return;
    }
    while (body.size() < clen) {
        char tmp[8192];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; break; }
        body.append(tmp, (size_t)n);
    }
    if (body.size() > clen) body.resize(clen);

    if (method == "GET" && path == "/health") {
        std::ostringstream out;
        out << "{\"ok\":true,\"version\":\"0.1\",\"project\":" << jsonStr(baseDir.string()) << "}";
        httpRespondRaw(fd, 200, out.str());
        return;
    }
    if (method == "POST" && path == "/run") {
        std::string perr;
        Json req = Json::parse(body, &perr);
        if (!perr.empty()) {
            httpRespondRaw(fd, 400, "{\"ok\":false,\"err\":\"bad request json\"}");
            return;
        }
        std::string cmdText = req.getStr2("cmd", "cmd");
        if (cmdText.empty()) {
            httpRespondRaw(fd, 400, "{\"ok\":false,\"err\":\"missing cmd string\"}");
            return;
        }
        auto t0 = std::chrono::steady_clock::now();
        std::string stdoutText = handleCommandCapture(cmdText, baseDir);
        auto t1 = std::chrono::steady_clock::now();
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::ostringstream out;
        out << "{\"ok\":true,\"stdout\":" << jsonStr(stdoutText)
            << ",\"stderr\":\"\",\"exit\":0,\"ms\":" << ms << "}";
        httpRespondRaw(fd, 200, out.str());
        return;
    }
    httpRespondRaw(fd, 404, "{\"ok\":false,\"err\":\"not found\"}");
}

static void startRelayServer(const fs::path& baseDir, int port) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        std::cerr << RED << "relay: socket() failed: " << std::strerror(errno) << RST << "\n";
        return;
    }
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << RED << "relay: bind 127.0.0.1:" << port << " failed: " << std::strerror(errno) << RST << "\n";
        ::close(srv);
        return;
    }
    if (::listen(srv, 4) < 0) {
        std::cerr << RED << "relay: listen failed: " << std::strerror(errno) << RST << "\n";
        ::close(srv);
        return;
    }
    std::cerr << GRN << "relay: listening on http://127.0.0.1:" << port << RST << "\n";
    for (;;) {
        sockaddr_in cli{};
        socklen_t clilen = sizeof(cli);
        int fd = ::accept(srv, (sockaddr*)&cli, &clilen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            std::cerr << RED << "relay: accept failed: " << std::strerror(errno) << RST << "\n";
            break;
        }
        handleRelayConn(fd, baseDir);
        ::close(fd);
    }
    ::close(srv);
}

void printHelp() {
    if (!g_plainMode) {
        tuiHeaderBox();
        std::cout << "\n";
    }
    std::cout <<
"Paste a JSON command. It runs the moment braces balance.\n"
"Slash: /help /clear /roots /plain /exit - Ctrl+L redraws the header\n"
"\n" << CYN << BOLD << "Tools:" << RST << "\n"
"  " << YEL << "sh"      << RST << "      shell_command   {\"t\":\"sh\",\"a\":{\"cmd\":\"make\",\"to\":30}}\n"
"  " << YEL << "search"  << RST << "  multi_search      {\"t\":\"search\",\"a\":{\"q\":[\"foo\"],\"paths\":[\"src\"],\"re\":true,\"ctx\":2}}\n"
"  " << YEL << "read"    << RST << "    multi_read        {\"t\":\"read\",\"a\":{\"r\":[{\"f\":\"main.cpp\",\"s\":1,\"e\":100,\"ln\":false}]}}\n"
"  " << YEL << "patch"   << RST << "   multi_patch       {\"t\":\"patch\",\"a\":{\"p\":[...]}}\n"
"  " << YEL << "edit"    << RST << "    file_edit         {\"t\":\"edit\",\"a\":{\"e\":[{\"f\":\"x\",\"mode\":\"create\",\"c\":\"hi\"}]}}\n"
"  " << YEL << "git"     << RST << "     git_tool          {\"t\":\"git\",\"a\":{\"a\":\"status\"}}\n"
"  " << YEL << "list"    << RST << "    list_tool          {\"t\":\"list\",\"a\":{\"path\":\"src\",\"depth\":2}}\n"
"  " << YEL << "find"    << RST << "    find_tool          {\"t\":\"find\",\"a\":{\"glob\":\"*.cpp\"}}\n"
"  " << YEL << "outline" << RST << " outline_tool       {\"t\":\"outline\",\"a\":{\"f\":\"main.cpp\"}}\n"
"  " << YEL << "recent"  << RST << "  recent_tool        {\"t\":\"recent\",\"a\":{\"min\":30}}\n"
"  " << YEL << "undo"    << RST << "    undo_tool          {\"t\":\"undo\",\"a\":{\"f\":\"main.cpp\"}}\n"
"  " << YEL << "create_file" << RST << " create_file_tool  {\"t\":\"create_file\",\"a\":{\"f\":\"x.py\",\"c\":\"...\"}}\n"
"  " << YEL << "create_directory" << RST << " create_dir/mkdir {\"t\":\"create_directory\",\"a\":{\"p\":\"src/new\"}}\n"
"  " << YEL << "cut"     << RST << "     cut_tool          {\"t\":\"cut\",\"a\":{\"f\":\"big.cpp\",\"s\":10,\"e\":80,\"to\":\"part.cpp\",\"mode\":\"create\"}}\n"
"  " << YEL << "extract" << RST << " extract_tool      {\"t\":\"extract\",\"a\":{\"f\":\"big.cpp\",\"name\":\"myFunc\",\"to\":\"part.cpp\",\"mode\":\"move\"}}\n"
"  " << YEL << "fileops" << RST << " fileops_tool      {\"t\":\"fileops\",\"a\":{\"ops\":[{\"op\":\"move\",\"f\":\"a\",\"to\":\"b\"}]}}\n"
"  " << YEL << "diagnostics" << RST << " diagnostics_tool {\"t\":\"diagnostics\",\"a\":{\"cmd\":\"make\",\"to\":60}}\n"
"  " << YEL << "add_dir" << RST << "   add_dir_tool     {\"t\":\"add_dir\",\"a\":{\"p\":\"/abs/dir\"}}\n"
"\n" << CYN << BOLD << "cut/extract:" << RST << "\n"
"  cut moves an explicit line range 's'-'e' from 'f' into 'to' (dst 'mode': append/prepend/insert_after/create).\n"
"  extract locates 'name' via the same detector outline uses, auto-finds its body (braces or indent),\n"
"  then cuts/copies it; 'mode':'move' removes from source, default 'copy' leaves source untouched.\n"
"  Both back up any file they modify to .mpt_backups first (undo_tool restores it).\n"
"\n" << CYN << BOLD << "fileops:" << RST << " ops: copy/cp, move/mv/rename, delete/rm (r:true for dirs), stat\n"
"\n" << CYN << BOLD << "diagnostics:" << RST << " runs 'cmd' like sh, plus parses GCC/Clang-style\n"
"  \"file:line:col: severity: message\" lines into a structured 'diags' array with error/warning counts.\n"
"\n" << CYN << BOLD << "workspaces:" << RST << " 1-3 dirs at launch (first is primary); add_dir\n"
"  adds more (cap 6). Sandbox = union of all roots; sh/git/diagnostics take \"dir\".\n"
"\n" << CYN << BOLD << "patch modes:" << RST << "\n"
"  search_replace: {\"f\":\"x\",\"o\":\"old\",\"n\":\"new\",\"occ\":1}\n"
"  replace_lines:  {\"f\":\"x\",\"mode\":\"replace_lines\",\"s\":10,\"e\":12,\"n\":\"new\"}\n"
"  delete_lines:   {\"f\":\"x\",\"mode\":\"delete_lines\",\"s\":10,\"e\":12}\n"
"  diff:           {\"f\":\"x\",\"mode\":\"diff\",\"diff\":\"@@ -10,3 +10,3 @@\\n ctx\\n-old\\n+new\\n ctx\"}\n"
"\n" << CYN << BOLD << "escape-free text (avoid JSON backslash-doubling):" << RST << "\n"
"  any of o/n/diff/content accept a base64 sibling instead: ob64, nb64, diffb64, cb64\n"
"  e.g. {\"f\":\"x\",\"ob64\":\"<base64>\",\"nb64\":\"<base64>\"} — base64 wins if both given.\n"
"\n" << CYN << BOLD << "echo & preview (see results before/without writing):" << RST << "\n"
"  add \"preview\":true at the top level of a patch/edit call to dry-run the whole batch.\n"
"  add \"echo\":true on an individual patch to get back the resulting lines after it applies\n"
"  (works with preview too, so you can see the hypothetical result without writing).\n"
"\n" << CYN << BOLD << "undo:" << RST << " patch/edit snapshot the pre-change file to .mpt_backups\n"
"  on first write. {\"t\":\"undo\",\"a\":{\"f\":\"x\"}} restores the most recent snapshot (n=1);\n"
"  {\"list\":true} lists snapshots; the undo itself is snapshotted, so undo again to redo.\n"
"  patch/edit results also include a top-level \"files\" array with a chk (hash), lines,\n"
"  and bytes per written file so you can confirm state without a follow-up read.\n"
"\n" << CYN << BOLD << "Batch (multiple tools at once):" << RST << "\n"
"  {\"calls\":[{\"t\":\"...\",\"a\":{...}}, ...]}\n"
"\n" << CYN << BOLD << "Multi-patch same file:" << RST << " line numbers refer to the ORIGINAL file;\n"
"  the tool tracks cumulative offsets and adjusts each subsequent patch automatically.\n"
"  One bad patch in a batch fails that entry only — the rest of the batch still runs.\n"
"\n" << CYN << BOLD << "Commands:" << RST << "  " << YEL << "help" << RST << " | " << YEL << "exit" << RST << "/" << YEL << "quit" << RST << "\n"
"\n" << CYN << BOLD << "Tip:" << RST << " for large payloads (>4KB), pipe via file:\n"
"  " << BOLD << "./nexon_code /path/to/project < request.json" << RST << "\n"
"\n";
}

// ===========================================================================
// Main
// ===========================================================================
int main(int argc, char* argv[]) {
    g_colorOut = isatty(STDOUT_FILENO);
    g_colorIn  = isatty(STDIN_FILENO);

    // Mitigation for chat apps whose paste/textbox handling mangles large or
    // heavily-decorated output (reported: works fine in Termux, truncates or
    // drops content in some Android AI apps). --plain strips ANSI colors and
    // the box-drawing banner entirely so the paste is just raw JSON; an
    // optional output cap wraps oversized results in a small JSON envelope
    // with paging guidance instead of dumping one huge blob.
    std::vector<std::string> posArgs;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--plain") { g_plainMode = true; continue; }
        if (a.rfind("--max-output=", 0) == 0) {
            try { g_maxOutputChars = (size_t)std::stoul(a.substr(13)); } catch (...) {}
            continue;
        }
        if (a.rfind("--max-input=", 0) == 0) {
            try { g_maxInputBytes = (size_t)std::stoul(a.substr(12)) * 1024 * 1024; } catch (...) {}
            continue;
        }
        posArgs.push_back(a);
    }
    if (const char* envPlain = std::getenv("TOOLS_PLAIN")) {
        std::string v = envPlain;
        if (!v.empty() && v != "0") g_plainMode = true;
    }
    if (std::getenv("NO_COLOR")) g_plainMode = true; // https://no-color.org convention
    if (const char* envMax = std::getenv("TOOLS_MAX_OUTPUT")) {
        try { g_maxOutputChars = (size_t)std::stoul(envMax); } catch (...) {}
    }
    if (const char* envMaxIn = std::getenv("TOOLS_MAX_INPUT")) {
        try { g_maxInputBytes = (size_t)std::stoul(envMaxIn) * 1024 * 1024; } catch (...) {}
    }
    if (g_plainMode) g_colorOut = false;

    fs::path baseDir;
    if (!posArgs.empty()) {
        std::string pathArg = posArgs[0];
        if (pathArg == "--help" || pathArg == "-h") {
            std::cout << "Usage: nexon_code [PROJECT_DIR ...] [--plain] [--max-output=N] [--max-input=MB] < commands.json\n"
                         "  PROJECT_DIR     - 1-3 project directories; the first is the primary workspace\n"
                         "                    (skips interactive prompt); add_dir registers more mid-session\n"
                         "  --plain         - no ANSI colors / box-drawing decoration, raw JSON only\n"
                         "                    (also: TOOLS_PLAIN=1 or NO_COLOR env var)\n"
                         "  --max-output=N  - cap a single result at N chars, wrapped with paging\n"
                         "                    guidance if exceeded (also: TOOLS_MAX_OUTPUT=N env var)\n"
                         "  --max-input=MB  - cap one pasted/piped JSON payload at MB megabytes\n"
                         "                    (default 64; also: TOOLS_MAX_INPUT=MB env var)\n"
                         "  If PROJECT_DIR is omitted, prompts interactively for up to 3 directories.\n"
                         "\n"
                         "  For large payloads (>4KB), pipe via file or redirect:\n"
                         "    ./nexon_code /path/to/project < patch_request.json\n"
                         "  Pipes have no tty canonical-mode 4KB line limit.\n";
            return 0;
        }
        if (posArgs.size() > 3) {
            std::cerr << RED << "Error: at most 3 project directories are supported (got "
                      << posArgs.size() << ")." << RST << "\n";
            return 1;
        }
        std::error_code ec;
        for (auto& pathArg : posArgs) {
            fs::path p = fs::absolute(pathArg, ec);
            if (!ec && fs::exists(p, ec) && fs::is_directory(p, ec)) {
                fs::path canon = fs::canonical(p, ec);
                if (ec) {
                    std::cerr << RED << "Error: cannot canonicalize " << pathArg << ": " << ec.message() << RST << "\n";
                    return 1;
                }
                g_roots.push_back(canon);
            } else {
                std::cerr << RED << "Error: '" << pathArg << "' is not a valid directory." << RST << "\n";
                return 1;
            }
        }
        baseDir = g_roots[0];
        g_primary = baseDir;
    } else {
        if (!g_plainMode) {
            printTuiBanner();
        }
        while (g_roots.size() < 3) {
            std::cout << BOLD << CYN << "❯ " << RST << CYN << "Enter project directory " << (g_roots.size() + 1) << " of up to 3"
                      << (g_roots.empty() ? "" : " (Enter to finish)") << ": " << RST;
            std::string path;
            if (!std::getline(std::cin, path)) return 0;
            path = trim(path);
            if (path.empty()) { if (!g_roots.empty()) break; continue; }
            if (toLower(path) == "q" || toLower(path) == "quit") return 0;
            std::error_code ec;
            fs::path p = fs::absolute(path, ec);
            if (!ec && fs::exists(p, ec) && fs::is_directory(p, ec)) {
                fs::path canon = fs::canonical(p, ec);
                if (!ec) { g_roots.push_back(canon); continue; }
            }
            std::cout << RED << "  Path not found or not a directory. Try again." << RST << "\n";
        }
        baseDir = g_roots[0];
        g_primary = baseDir;
        std::cout << GRN << "✓ Using " << g_roots.size() << " workspace" << (g_roots.size() == 1 ? "" : "s")
                  << "; primary: " << baseDir.string() << RST << "\n";
        for (size_t i = 1; i < g_roots.size(); ++i)
            std::cout << GRN << "  extra: " << g_roots[i].string() << RST << "\n";
    }

    // Start embedded HTTP relay now that project root(s) are known.
    {
        int relayPort = 8787;
        if (const char* envPort = std::getenv("RELAY_PORT")) {
            try { relayPort = std::stoi(envPort); } catch (...) {}
        }
        std::thread(startRelayServer, baseDir, relayPort).detach();
    }

    bool isTty = false;
    struct termios origTermios;
    bool termiosChanged = false;
    if (isatty(STDIN_FILENO)) {
        isTty = true;
        if (tcgetattr(STDIN_FILENO, &origTermios) == 0) {
            struct termios raw = origTermios;
            raw.c_lflag &= ~(ICANON | ECHO);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
                termiosChanged = true;
            }
        }
    }

    if (!isTty && !posArgs.empty()) {
        // Piped mode: no help banner, just process
    } else if (isTty && !g_plainMode) {
        // TUI footer: separator + first prompt. The full tool reference
        // prints on the 'help' command, not at every launch.
        tuiSep();
        tuiPrompt();
    } else {
        std::cout << " type 'help' for the tool reference, 'exit' to quit\n\n";
    }

    JsonAccumulator acc;
    acc.reset();
    bool collecting = false;

    if (isTty && termiosChanged) {
        char buf[4096];
        std::string pendingInput;
        std::string currentLine;
        static constexpr size_t ECHO_LIMIT = 64 * 1024; // stop full-echo past 64KB
        bool echoLarge = false;
        size_t nextProgressAt = 0;

        while (true) {
                    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (n == 0) break;

                    for (ssize_t k = 0; k < n; ++k) {
                        char c = buf[k];
                        if (!collecting) {
                            if (c == '\n' || c == '\r') {
                                std::string line = pendingInput;
                                pendingInput.clear();
                                std::string trimmed = trim(line);
                                if (trimmed.empty()) continue;
                                std::string lower = toLower(trimmed);
                                if (lower == "exit" || lower == "quit" || lower == "/exit" || lower == "/quit") goto done;
                                if (lower == "help" || lower == "/help") { printHelp(); tuiPrompt(); continue; }
                                if (lower == "/clear") { printTuiBanner(); tuiPrompt(); continue; }
                                if (lower == "/roots") {
                                    for (size_t i = 0; i < g_roots.size(); ++i)
                                        std::cout << GRN << (i == 0 ? "  primary: " : "  extra:   ") << g_roots[i].string() << RST << "\n";
                                    tuiPrompt();
                                    continue;
                                }
                                if (lower == "/plain") {
                                    g_plainMode = !g_plainMode;
                                    g_colorOut = isatty(STDOUT_FILENO) && !g_plainMode;
                                    std::cout << YEL << "[nexon] plain mode " << (g_plainMode ? "on" : "off") << RST << "\n";
                                    tuiPrompt();
                                    continue;
                                }
                                if (lower == "/stop" || lower == "stop" || lower == "/cancel" || lower == "/reset") {
                                    std::cout << YEL << "[nexon] Session ready. Input buffer clean." << RST << "\n";
                                    tuiPrompt();
                                    continue;
                                }
                                if (!trimmed.empty() && trimmed[0] == '/') {
                                    std::cout << YEL << "[nexon] Unknown slash command. Try /help /clear /roots /plain /stop /exit" << RST << "\n";
                                    tuiPrompt();
                                    continue;
                                }
                                size_t bracePos = line.find('{');
                                size_t bracketPos = line.find('[');
                                if (bracePos == std::string::npos && bracketPos == std::string::npos) {
                                    std::cout << YEL << "[nexon] Not JSON. Type 'help' or 'exit'." << RST << "\n";
                                    tuiPrompt();
                                    continue;
                                }
                                collecting = true;
                                acc.reset();
                                echoLarge = false;
                                nextProgressAt = 0;
                                currentLine.clear();
                                std::cout << "\n" << BG_CYAN << BOLD << " ▲ INPUT " << RST << " " << CYN << "(pasting JSON...)" << RST << "\n";
                                std::cout << CYN << "┌────────────────────────────────────────────────────" << RST << "\n";
                                std::cout << CYN << "│" << RST;
                                size_t bp = std::min(bracePos, bracketPos); // whichever opener appears first
                                for (size_t i = bp; i < line.size(); ++i) {
                                    acc.feed(line[i]);
                                    if (acc.buffer.size() <= ECHO_LIMIT) {
                                        std::cout << line[i];
                                    } else {
                                        if (!echoLarge) {
                                            echoLarge = true;
                                            nextProgressAt = acc.buffer.size();
                                            std::cout << "\n" << CYN << "  (large paste: showing progress instead of full echo)" << RST << "\n";
                                        }
                                        if (acc.buffer.size() >= nextProgressAt) {
                                            std::cout << "\r" << CYN << "  received " << humanSize(acc.buffer.size())
                                                       << " / cap " << humanSize(acc.MAX_BUFFER) << "   " << RST;
                                            std::cout.flush();
                                            nextProgressAt = acc.buffer.size() + 256 * 1024;
                                        }
                                    }
                                    if (acc.overflow) break;
                                }
                                if (acc.overflow) {
                                    std::cout << "\n" << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                                    printError("Input buffer exceeded " + humanSize(acc.MAX_BUFFER) + " without completing",
                                              "Raise it with --max-input=MB or TOOLS_MAX_INPUT=MB, or pipe from a file:\n  ./nexon_code " +
                                              baseDir.string() + " < request.json");
                                    collecting = false;
                                    acc.reset();
                                    echoLarge = false;
                                    currentLine.clear();
                                    tuiPrompt();
                                    continue;
                                }
                                if (acc.complete) {
                                    std::cout << "\n" << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                                    std::cout.flush();
                                    handleCommand(acc.buffer, baseDir);
                                    collecting = false;
                                    acc.reset();
                                    currentLine.clear();
                                    std::cout << "\n";
                                    tuiPrompt();
                                }
                                continue;
                            }
                            if (c == 12) { // Ctrl+L: redraw header + prompt
                                printTuiBanner();
                                tuiPrompt();
                                if (!pendingInput.empty()) { std::cout << pendingInput; std::cout.flush(); }
                                continue;
                            }
                            if (c == 127 || c == 8) { // TUI line editing: backspace
                                if (!pendingInput.empty()) {
                                    pendingInput.pop_back();
                                    std::cout << "\b \b";
                                    std::cout.flush();
                                }
                                continue;
                            }
                            if ((unsigned char)c >= 32) { std::cout << c; std::cout.flush(); }
                            pendingInput += c;
                            if (pendingInput.size() > g_maxInputBytes) {
                                printError("Input line exceeded " + humanSize(g_maxInputBytes) + " without a newline",
                                          "Raise the cap with --max-input=MB or TOOLS_MAX_INPUT=MB, or pipe from a file:\n  ./nexon_code " +
                                          baseDir.string() + " < request.json");
                                pendingInput.clear();
                            }
                            continue;
                        }
                        if (!acc.started && (c == '\n' || c == '\r' || c == ' ' || c == '\t')) {
                            continue;
                        }

                        if (c == '\n' || c == '\r') {
                            std::string lineTrimmed = trim(currentLine);
                            std::string lineLower = toLower(lineTrimmed);
                            currentLine.clear();
                            if (lineLower == "/stop" || lineLower == "stop" || lineLower == "/cancel" || lineLower == "/reset") {
                                std::cout << "\n" << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                                std::cout << YEL << "[nexon] Input stopped/cancelled. Cleared incomplete JSON payload." << RST << "\n";
                                collecting = false;
                                acc.reset();
                                echoLarge = false;
                                pendingInput.clear();
                                tuiPrompt();
                                continue;
                            }
                        } else {
                            currentLine += c;
                        }

                        acc.feed(c);
                        if (acc.buffer.size() <= ECHO_LIMIT) {
                            if (c == '\n') std::cout << "\n" << CYN << "│" << RST;
                            else if ((unsigned char)c >= 32 || c == '\t') std::cout << c;
                        } else {
                            if (!echoLarge) {
                                echoLarge = true;
                                nextProgressAt = acc.buffer.size();
                                std::cout << "\n" << CYN << "  (large paste: showing progress instead of full echo)" << RST << "\n";
                            }
                            if (acc.buffer.size() >= nextProgressAt) {
                                std::cout << "\r" << CYN << "  received " << humanSize(acc.buffer.size())
                                           << " / cap " << humanSize(acc.MAX_BUFFER) << "   " << RST;
                                std::cout.flush();
                                nextProgressAt = acc.buffer.size() + 256 * 1024;
                            }
                        }

                        if (acc.overflow) {
                            std::cout << "\n" << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                            printError("Input buffer exceeded " + humanSize(acc.MAX_BUFFER) + " without completing",
                                      "The input was likely truncated (tty canonical-mode limit, dropped paste, or memory), "
                                      "or the payload is genuinely bigger than the cap. Raise it with --max-input=MB or "
                                      "TOOLS_MAX_INPUT=MB, or write the JSON to a file and pipe it in:\n  ./nexon_code " +
                                      baseDir.string() + " < request.json");
                            collecting = false;
                            acc.reset();
                            echoLarge = false;
                            currentLine.clear();
                            tuiPrompt();
                            continue;
                        }
                        if (acc.complete) {
                            std::cout << "\n" << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                            std::cout.flush();
                            handleCommand(acc.buffer, baseDir);
                            collecting = false;
                            acc.reset();
                            currentLine.clear();
                            std::cout << "\n";
                            tuiPrompt();
                        }
                    }
                }
                done:;
                if (collecting && acc.started) {
                    std::cout << "\n" << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                    printError("Input ended (EOF) before JSON braces balanced",
                              "Received " + std::to_string(acc.buffer.size()) +
                              " byte(s) that never formed valid JSON (unbalanced { }). Nothing was executed.");
                }
            } else {
                std::string line;
                std::string pendingInput;
                while (std::getline(std::cin, line)) {
                    std::string trimmed = trim(line);
                    std::string lower = toLower(trimmed);
                    if (lower == "/stop" || lower == "stop" || lower == "/cancel" || lower == "/reset") {
                        if (collecting) {
                            if (isTty) std::cout << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                            std::cout << YEL << "[nexon] Input stopped/cancelled. Cleared incomplete JSON payload." << RST << "\n";
                            collecting = false;
                            acc.reset();
                            if (isTty) tuiPrompt();
                            continue;
                        } else {
                            if (isTty) {
                                std::cout << YEL << "[nexon] Session ready. Input buffer clean." << RST << "\n";
                                tuiPrompt();
                            }
                            continue;
                        }
                    }
                    if (!collecting) {
                        if (trimmed.empty()) continue;
                        if (lower == "exit" || lower == "quit") break;
                        if (lower == "help") { printHelp(); continue; }
                        if (line.find('{') == std::string::npos && line.find('[') == std::string::npos) {
                            if (!isTty) {
                                std::cerr << YEL << "[nexon] Non-JSON line ignored: " << trimmed.substr(0, 80) << RST << "\n";
                            } else {
                                std::cout << YEL << "[nexon] Not JSON. Type 'help' or 'exit'." << RST << "\n";
                            }
                            continue;
                        }
                        collecting = true;
                        acc.reset();
                        if (isTty) {
                            std::cout << "\n" << BG_CYAN << BOLD << " ▲ INPUT " << RST << " " << CYN << "(pasting JSON...)" << RST << "\n";
                            std::cout << CYN << "┌────────────────────────────────────────────────────" << RST << "\n";
                            std::cout << CYN << "│" << RST << line << "\n";
                        }
                    } else if (isTty) {
                        std::cout << CYN << "│" << RST << line << "\n";
                    }
                    for (char c : line) acc.feed(c);
                    acc.feed('\n');

                    if (acc.overflow) {
                        if (isTty) std::cout << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                        printError("Input buffer exceeded " + humanSize(acc.MAX_BUFFER) + " without completing",
                                  "Raise it with --max-input=MB or TOOLS_MAX_INPUT=MB, or pipe via file: ./nexon_code " +
                                  baseDir.string() + " < request.json");
                        collecting = false;
                        acc.reset();
                        continue;
                    }
                    if (acc.complete) {
                        if (isTty) std::cout << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                        std::cout.flush();
                        handleCommand(acc.buffer, baseDir);
                        collecting = false;
                        acc.reset();
                    }
                }
                if (collecting && acc.started) {
                    if (isTty) std::cout << CYN << "└────────────────────────────────────────────────────" << RST << "\n";
                    printError("Input ended (EOF) before JSON braces balanced",
                              "Received " + std::to_string(acc.buffer.size()) +
                              " byte(s) that never formed valid JSON (unbalanced { }). "
                              "Nothing was executed. Check the payload is complete and well-formed.");
                }
            }

    if (termiosChanged) {
        tcsetattr(STDIN_FILENO, TCSANOW, &origTermios);
    }
    if (isTty) {
        std::cout << MAG << "Goodbye." << RST << "\n";
    }
    return 0;
}
