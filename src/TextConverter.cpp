#include "TextConverter.h"

#include "jianfan_embedded.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// GW2 UI strings are UTF-16 and Chinese glyphs live in the CJK Unified
// Ideographs block. We only attempt a dictionary lookup when the current
// character falls in this range, matching the original lang5 shellcode.
constexpr char16_t kCjkStart = 0x4E00;
constexpr char16_t kCjkEnd = 0x9FFF;

// Upper bound on the working buffer, in UTF-16 code units (including the null
// terminator). Strings longer than this are left unconverted rather than risk a
// stack overflow. GW2 UI strings are far shorter than this in practice.
constexpr size_t kMaxStringUnits = 8192;

struct Rule {
    std::u16string in;
    std::u16string out;
};

// Rules grouped by the last code unit of their input, each bucket sorted by
// input length descending so the longest match at a position wins. Built once
// at load time and only read afterwards, so lookups are reentrant/thread-safe.
std::unordered_map<char16_t, std::vector<Rule>> g_categories;
bool g_ready = false;

// ---- UTF-8 / UTF-16 helpers -------------------------------------------------

void AppendCodepoint(std::u16string& out, uint32_t cp) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char16_t>(cp));
    } else {
        cp -= 0x10000;
        out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
}

// ---- Minimal JSON parser ----------------------------------------------------
//
// Only supports what the dictionary files use: an array of objects whose string
// values we care about ("i" and "o"). Other value types are skipped. Malformed
// input stops parsing gracefully rather than throwing.

struct JsonParser {
    const char* p;
    const char* end;

    bool AtEnd() const { return p >= end; }

    void SkipWs() {
        while (p < end) {
            char c = *p;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p;
            } else {
                break;
            }
        }
    }

    // Parse a JSON string (p must point at the opening quote). Decodes escapes
    // and passes raw UTF-8 bytes straight through to UTF-16.
    bool ParseString(std::u16string& out) {
        out.clear();
        if (AtEnd() || *p != '"') {
            return false;
        }
        ++p;
        while (p < end) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') {
                ++p;
                return true;
            }
            if (c == '\\') {
                ++p;
                if (AtEnd()) {
                    return false;
                }
                char e = *p++;
                switch (e) {
                case '"': out.push_back(u'"'); break;
                case '\\': out.push_back(u'\\'); break;
                case '/': out.push_back(u'/'); break;
                case 'b': out.push_back(u'\b'); break;
                case 'f': out.push_back(u'\f'); break;
                case 'n': out.push_back(u'\n'); break;
                case 'r': out.push_back(u'\r'); break;
                case 't': out.push_back(u'\t'); break;
                case 'u': {
                    if (end - p < 4) {
                        return false;
                    }
                    uint32_t v = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = *p++;
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
                        else return false;
                    }
                    // Keep surrogate halves as-is; a following \u low surrogate
                    // naturally pairs up as a second code unit.
                    out.push_back(static_cast<char16_t>(v));
                    break;
                }
                default:
                    return false;
                }
                continue;
            }
            // Raw byte: decode a UTF-8 sequence.
            uint32_t cp = 0;
            int extra = 0;
            if (c < 0x80) {
                cp = c;
            } else if ((c & 0xE0) == 0xC0) {
                cp = c & 0x1F;
                extra = 1;
            } else if ((c & 0xF0) == 0xE0) {
                cp = c & 0x0F;
                extra = 2;
            } else if ((c & 0xF8) == 0xF0) {
                cp = c & 0x07;
                extra = 3;
            } else {
                // Invalid lead byte; emit replacement and resync.
                out.push_back(u'�');
                ++p;
                continue;
            }
            ++p;
            bool ok = true;
            for (int i = 0; i < extra; ++i) {
                if (AtEnd() || (static_cast<unsigned char>(*p) & 0xC0) != 0x80) {
                    ok = false;
                    break;
                }
                cp = (cp << 6) | (static_cast<unsigned char>(*p) & 0x3F);
                ++p;
            }
            if (!ok) {
                out.push_back(u'�');
                continue;
            }
            AppendCodepoint(out, cp);
        }
        return false;
    }

    // Skip an arbitrary JSON value we do not care about.
    bool SkipValue() {
        SkipWs();
        if (AtEnd()) {
            return false;
        }
        char c = *p;
        if (c == '"') {
            std::u16string tmp;
            return ParseString(tmp);
        }
        if (c == '{' || c == '[') {
            char open = c;
            char close = (c == '{') ? '}' : ']';
            ++p;
            int depth = 1;
            while (p < end && depth > 0) {
                char d = *p;
                if (d == '"') {
                    std::u16string tmp;
                    if (!ParseString(tmp)) {
                        return false;
                    }
                    continue;
                }
                if (d == open) {
                    ++depth;
                } else if (d == close) {
                    --depth;
                }
                ++p;
            }
            return depth == 0;
        }
        // number, true, false, null: read until a structural/whitespace char.
        while (p < end) {
            char d = *p;
            if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' ||
                d == '\n' || d == '\r') {
                break;
            }
            ++p;
        }
        return true;
    }

    // Parse one object, extracting the "i"/"o" string fields.
    bool ParseObject(std::u16string& in, std::u16string& out) {
        in.clear();
        out.clear();
        SkipWs();
        if (AtEnd() || *p != '{') {
            return false;
        }
        ++p;
        while (true) {
            SkipWs();
            if (AtEnd()) {
                return false;
            }
            if (*p == '}') {
                ++p;
                return true;
            }
            std::u16string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWs();
            if (AtEnd() || *p != ':') {
                return false;
            }
            ++p;
            if (key == u"i") {
                SkipWs();
                if (!ParseString(in)) {
                    return false;
                }
            } else if (key == u"o") {
                SkipWs();
                if (!ParseString(out)) {
                    return false;
                }
            } else {
                if (!SkipValue()) {
                    return false;
                }
            }
            SkipWs();
            if (AtEnd()) {
                return false;
            }
            if (*p == ',') {
                ++p;
                continue;
            }
            if (*p == '}') {
                ++p;
                return true;
            }
            return false;
        }
    }
};

// Parse a whole dictionary file into (in -> out), later entries overriding
// earlier ones. Returns false if the top-level array cannot be read at all.
bool ParseDictionaryFile(const std::string& text,
                         std::unordered_map<std::u16string, std::u16string>& best) {
    JsonParser parser{text.data(), text.data() + text.size()};
    parser.SkipWs();
    if (parser.AtEnd() || *parser.p != '[') {
        return false;
    }
    ++parser.p;
    while (true) {
        parser.SkipWs();
        if (parser.AtEnd()) {
            return false;
        }
        if (*parser.p == ']') {
            ++parser.p;
            return true;
        }
        std::u16string in;
        std::u16string out;
        if (!parser.ParseObject(in, out)) {
            return false;
        }
        if (!in.empty() && !out.empty()) {
            best[in] = out;
        }
        parser.SkipWs();
        if (parser.AtEnd()) {
            return false;
        }
        if (*parser.p == ',') {
            ++parser.p;
            continue;
        }
        if (*parser.p == ']') {
            ++parser.p;
            return true;
        }
        return false;
    }
}

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        file.read(&out[0], size);
    }
    return static_cast<bool>(file) || file.eof();
}

std::string JoinPath(const char* dir, const char* name) {
    std::string path = dir ? dir : "";
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path.push_back('\\');
    }
    path += name;
    return path;
}

// Load one optional override file into the dedup map; absence is not an error.
// Returns true if the file was read and parsed.
bool LoadInto(const char* dir, const char* name, Lang5LogFn logFn,
              std::unordered_map<std::u16string, std::u16string>& best) {
    std::string path = JoinPath(dir, name);
    std::string text;
    if (!ReadFile(path, text)) {
        return false;
    }
    if (!ParseDictionaryFile(text, best)) {
        if (logFn) {
            char msg[320];
            std::snprintf(msg, sizeof(msg), "Failed to parse dictionary: %s", path.c_str());
            logFn(LANG5_LOG_WARNING, msg);
        }
        return false;
    }
    return true;
}

} // namespace

int Lang5LoadDictionaries(const char* dir, Lang5LogFn logFn) {
    Lang5ClearDictionaries();

    // The base dictionary ships inside the DLL; files on disk are optional
    // overrides layered on top (later wins on duplicate inputs).
    std::unordered_map<std::u16string, std::u16string> best;
    std::string embedded(reinterpret_cast<const char*>(kEmbeddedJianfanJson),
                         kEmbeddedJianfanJsonSize);
    if (!ParseDictionaryFile(embedded, best) && logFn) {
        logFn(LANG5_LOG_WARNING, "Failed to parse the embedded base dictionary.");
    }

    if (dir) {
        LoadInto(dir, "jianfan.json", logFn, best);
        LoadInto(dir, "add.json", logFn, best);
        LoadInto(dir, "user.json", logFn, best);
    }

    if (best.empty()) {
        return 0;
    }

    for (const auto& entry : best) {
        char16_t key = entry.first.back();
        g_categories[key].push_back(Rule{entry.first, entry.second});
    }

    // Longest input first within each bucket so multi-character runs match
    // before their single-character suffixes.
    for (auto& bucket : g_categories) {
        std::vector<Rule>& rules = bucket.second;
        for (size_t i = 1; i < rules.size(); ++i) {
            Rule current = rules[i];
            size_t j = i;
            while (j > 0 && rules[j - 1].in.size() < current.in.size()) {
                rules[j] = rules[j - 1];
                --j;
            }
            rules[j] = current;
        }
    }

    g_ready = !g_categories.empty();

    if (logFn) {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "Loaded %zu conversion rules in %zu buckets.",
                      best.size(), g_categories.size());
        logFn(LANG5_LOG_INFO, msg);
    }
    return static_cast<int>(best.size());
}

bool Lang5DictionariesReady() {
    return g_ready;
}

void Lang5ClearDictionaries() {
    g_categories.clear();
    g_ready = false;
}

extern "C" void Lang5ConvertStringInPlace(char16_t* s) {
    if (!s || !g_ready) {
        return;
    }

    // Working copy built one character at a time. When the character just
    // appended completes a dictionary input, that suffix is rewritten to the
    // traditional output, mirroring the original lang5 greedy suffix match.
    char16_t buf[kMaxStringUnits];
    size_t hlen = 0;
    bool replaced = false;

    for (size_t i = 0;; ++i) {
        char16_t c = s[i];
        if (hlen >= kMaxStringUnits - 1) {
            return; // too long: leave the original string untouched
        }
        buf[hlen++] = c;
        if (c == 0) {
            break; // trailing null appended; done scanning
        }
        if (c < kCjkStart || c > kCjkEnd) {
            continue;
        }

        auto it = g_categories.find(c);
        if (it == g_categories.end()) {
            continue;
        }

        for (const Rule& rule : it->second) {
            size_t inLen = rule.in.size();
            if (hlen < inLen) {
                continue;
            }
            if (std::memcmp(&buf[hlen - inLen], rule.in.data(),
                            inLen * sizeof(char16_t)) != 0) {
                continue;
            }
            size_t outLen = rule.out.size();
            if (hlen - inLen + outLen >= kMaxStringUnits - 1) {
                return; // replacement would overflow the working buffer
            }
            hlen -= inLen;
            std::memcpy(&buf[hlen], rule.out.data(), outLen * sizeof(char16_t));
            hlen += outLen;
            replaced = true;
            break;
        }
    }

    if (!replaced) {
        return;
    }

    // hlen counts the trailing null, so this rewrites the terminator too.
    std::memcpy(s, buf, hlen * sizeof(char16_t));
}
