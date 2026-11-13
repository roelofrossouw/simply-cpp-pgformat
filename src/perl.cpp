#include "perl.h"

#define PCRE2_CODE_UNIT_WIDTH 32
#include <pcre2.h>

#include <algorithm>
#include <clocale>
#include <cwctype>
#include <stdexcept>
#include <unordered_map>

#include <locale.h>
#include <wctype.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace sc::pgformat::detail {
    namespace {
        void append_utf8(std::string &out, char32_t c) {
            if (c < 0x80) {
                out += static_cast<char>(c);
            } else if (c < 0x800) {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (c >> 18));
                out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
        }

        // A locale for case mapping beyond ASCII, made once and never installed globally.
        locale_t case_locale() {
            static const locale_t locale = [] {
                for (const char *name: {"C.UTF-8", "C.utf8", "en_US.UTF-8", "en_US.utf8", "UTF-8"})
                    if (const locale_t l = newlocale(LC_CTYPE_MASK, name, nullptr)) return l;
                return static_cast<locale_t>(nullptr);
            }();
            return locale;
        }

        char32_t lower(const char32_t c) {
            if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
            const auto l = case_locale();
            return l ? static_cast<char32_t>(towlower_l(static_cast<wint_t>(c), l)) : c;
        }

        char32_t upper(const char32_t c) {
            if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
            const auto l = case_locale();
            return l ? static_cast<char32_t>(towupper_l(static_cast<wint_t>(c), l)) : c;
        }

        struct cache_key_hash {
            std::size_t operator()(const str &s) const { return std::hash<str>{}(s); }
        };
    }

    str u32(const std::string_view utf8) {
        str out;
        out.reserve(utf8.size());
        const auto *p = reinterpret_cast<const unsigned char *>(utf8.data());
        const auto *end = p + utf8.size();
        while (p < end) {
            const unsigned char c = *p;
            int length = 0;
            char32_t code = 0;
            if (c < 0x80) {
                out += c;
                ++p;
                continue;
            }
            if ((c & 0xE0) == 0xC0) length = 2, code = c & 0x1F;
            else if ((c & 0xF0) == 0xE0) length = 3, code = c & 0x0F;
            else if ((c & 0xF8) == 0xF0) length = 4, code = c & 0x07;
            bool good = length > 0 && end - p >= length;
            for (int i = 1; good && i < length; ++i) {
                if ((p[i] & 0xC0) != 0x80) good = false;
                else code = (code << 6) | (p[i] & 0x3F);
            }
            // Overlong forms, surrogates and values past U+10FFFF aren't characters either.
            if (good && ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
                         (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF))
                good = false;
            if (good) {
                out += code;
                p += length;
            } else {
                out += U'�';
                ++p;
            }
        }
        return out;
    }

    std::string u8(const sv text) {
        std::string out;
        out.reserve(text.size());
        for (const char32_t c: text) append_utf8(out, c);
        return out;
    }

    regex::regex(const sv pattern, const std::string_view flags) {
        uint32_t options = PCRE2_UTF | PCRE2_UCP;
        for (const char f: flags) {
            if (f == 'i') options |= PCRE2_CASELESS;
            else if (f == 's') options |= PCRE2_DOTALL;
            else if (f == 'x') options |= PCRE2_EXTENDED;
            else if (f == 'm') options |= PCRE2_MULTILINE;
        }
        int error = 0;
        PCRE2_SIZE offset = 0;
        auto *code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), options, &error, &offset, nullptr);
        if (!code) {
            PCRE2_UCHAR message[256];
            pcre2_get_error_message(error, message, 256);
            std::string text;
            for (const auto *m = message; *m; ++m) append_utf8(text, static_cast<char32_t>(*m));
            throw std::invalid_argument("sc::pgformat: invalid regular expression /" + u8(pattern) + "/ at " +
                                        std::to_string(offset) + ": " + text);
        }
        pcre2_jit_compile(code, PCRE2_JIT_COMPLETE); // falls back to the interpreter when it can't
        uint32_t count = 0;
        pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &count);
        code_ = code;
        groups_ = static_cast<int>(count);
    }

    regex::~regex() { pcre2_code_free(static_cast<pcre2_code *>(code_)); }

    const regex &re(const sv pattern, const std::string_view flags) {
        thread_local std::unordered_map<str, std::unique_ptr<regex>, cache_key_hash> cache;
        str key{pattern};
        key += U'\0';
        for (const char f: flags) key += static_cast<char32_t>(f);
        auto found = cache.find(key);
        if (found == cache.end()) found = cache.emplace(std::move(key), std::make_unique<regex>(pattern, flags)).first;
        return *found->second;
    }

    const str &match::operator[](const std::size_t n) const {
        static const str empty;
        return n < groups_.size() && groups_[n] ? *groups_[n] : empty;
    }

    match find(const sv subject, const regex &pattern, const std::size_t offset, const unsigned options) {
        match result;
        if (offset > subject.size()) return result;
        auto *code = static_cast<pcre2_code *>(pattern.code());
        thread_local pcre2_match_data *data = nullptr;
        thread_local uint32_t data_pairs = 0;
        if (!data || data_pairs < static_cast<uint32_t>(pattern.groups() + 1)) {
            if (data) pcre2_match_data_free(data);
            data_pairs = std::max<uint32_t>(64, pattern.groups() + 1);
            data = pcre2_match_data_create(data_pairs, nullptr);
        }
        // Perl's regular expressions have no match, depth or heap limit; some of pgFormatter's
        // backtrack a lot on a big file (pg-strings.sql), past PCRE2's defaults.
        thread_local pcre2_match_context *context = [] {
            auto *c = pcre2_match_context_create(nullptr);
            pcre2_set_match_limit(c, 0xFFFFFFFFu);
            pcre2_set_depth_limit(c, 0xFFFFFFFFu);
            pcre2_set_heap_limit(c, 0xFFFFFFFFu);
            return c;
        }();
        const int rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(), offset, options, data, context);
        if (rc < 0) {
            if (rc == PCRE2_ERROR_NOMATCH) return result;
            PCRE2_UCHAR message[256];
            pcre2_get_error_message(rc, message, 256);
            std::string text;
            for (const auto *m = message; *m; ++m) text += static_cast<char>(*m);
            throw std::runtime_error("sc::pgformat: matching failed: " + text);
        }
        const PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(data);
        result.ok_ = true;
        result.begin_ = ovector[0];
        result.end_ = ovector[1];
        result.groups_.resize(pattern.groups() + 1);
        for (int i = 0; i <= pattern.groups(); ++i) {
            if (i < rc && ovector[2 * i] != PCRE2_UNSET)
                result.groups_[i] = str{subject.substr(ovector[2 * i], ovector[2 * i + 1] - ovector[2 * i])};
        }
        return result;
    }

    namespace {
        // The next match of a global search: after a match of no width, one of no width at the same
        // place isn't allowed (Perl's and PCRE2's rule), so it tries for a longer one there first.
        match next_match(const sv subject, const regex &pattern, std::size_t &offset, bool &last_empty) {
            while (offset <= subject.size()) {
                if (last_empty) {
                    auto m = find(subject, pattern, offset, PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED);
                    if (m) {
                        last_empty = m.begin() == m.end();
                        offset = m.end();
                        return m;
                    }
                    if (offset >= subject.size()) return {};
                    ++offset;
                    last_empty = false;
                }
                auto m = find(subject, pattern, offset);
                if (!m) return {};
                last_empty = m.begin() == m.end();
                offset = m.end();
                return m;
            }
            return {};
        }
    }

    std::vector<str> find_all(const sv subject, const regex &pattern) {
        std::vector<str> out;
        std::size_t offset = 0;
        bool last_empty = false;
        while (true) {
            auto m = next_match(subject, pattern, offset, last_empty);
            if (!m) break;
            if (pattern.groups() == 0) out.push_back(m[0]);
            else
                for (int i = 1; i <= pattern.groups(); ++i) out.push_back(m[i]);
        }
        return out;
    }

    bool subst(str &subject, const regex &pattern, const replacer &replacement, match *matched) {
        auto m = find(subject, pattern);
        if (!m) return false;
        const str text = replacement(m);
        subject.replace(m.begin(), m.end() - m.begin(), text);
        if (matched) *matched = std::move(m);
        return true;
    }

    std::size_t subst_all(str &subject, const regex &pattern, const replacer &replacement) {
        str out;
        std::size_t offset = 0, copied = 0, count = 0;
        bool last_empty = false;
        while (true) {
            auto m = next_match(subject, pattern, offset, last_empty);
            if (!m) break;
            out.append(subject, copied, m.begin() - copied);
            out += replacement(m);
            copied = m.end();
            ++count;
        }
        if (count) {
            out.append(subject, copied, str::npos);
            subject = std::move(out);
        }
        return count;
    }

    replacer with(const sv replacement) {
        // Split once into literal parts and group numbers.
        struct part {
            str text;
            int group = -1;
        };
        std::vector<part> parts;
        str literal;
        for (std::size_t i = 0; i < replacement.size(); ++i) {
            if (replacement[i] == U'$' && i + 1 < replacement.size()) {
                std::size_t j = i + 1;
                const bool braced = replacement[j] == U'{';
                if (braced) ++j;
                const std::size_t digits = j;
                while (j < replacement.size() && replacement[j] >= U'0' && replacement[j] <= U'9') ++j;
                if (j > digits && (!braced || (j < replacement.size() && replacement[j] == U'}'))) {
                    if (!literal.empty()) parts.push_back({std::move(literal)}), literal.clear();
                    int n = 0;
                    for (std::size_t k = digits; k < j; ++k) n = n * 10 + static_cast<int>(replacement[k] - U'0');
                    parts.push_back({{}, n});
                    i = braced ? j : j - 1;
                    continue;
                }
            }
            literal += replacement[i];
        }
        if (!literal.empty()) parts.push_back({std::move(literal)});
        return [parts](const match &m) {
            str out;
            for (const auto &p: parts) out += p.group >= 0 ? m[p.group] : p.text;
            return out;
        };
    }

    std::vector<str> split(const regex &pattern, const sv subject, const int limit) {
        std::vector<str> out;
        std::size_t offset = 0, field = 0;
        bool last_empty = false;
        while (limit <= 0 || static_cast<int>(out.size()) < limit - 1) {
            if (offset > subject.size()) break;
            auto m = next_match(subject, pattern, offset, last_empty);
            if (!m) break;
            // A match of no width at the start, or at the end, splits nothing off.
            if (m.begin() == m.end() && (m.begin() == 0 || m.begin() >= subject.size())) continue;
            out.emplace_back(subject.substr(field, m.begin() - field));
            for (int i = 1; i <= pattern.groups(); ++i) out.push_back(m[i]);
            field = m.end();
        }
        out.emplace_back(subject.substr(std::min(field, subject.size())));
        if (limit == 0)
            while (!out.empty() && out.back().empty()) out.pop_back();
        return out;
    }

    str lc(const sv text) {
        str out{text};
        for (auto &c: out) c = lower(c);
        return out;
    }

    str uc(const sv text) {
        str out{text};
        for (auto &c: out) c = upper(c);
        return out;
    }

    str ucfirst(const sv text) {
        str out{text};
        if (!out.empty()) out[0] = upper(out[0]);
        return out;
    }

    str quotemeta(const sv text) {
        str out;
        out.reserve(text.size() * 2);
        for (const char32_t c: text) {
            const bool word = (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || (c >= U'0' && c <= U'9') || c == U'_';
            if (c < 0x80 && !word) out += U'\\';
            out += c;
        }
        return out;
    }

    str repeat(const sv text, const long times) {
        str out;
        for (long i = 0; i < times; ++i) out += text;
        return out;
    }

    str number(const long value) {
        const auto text = std::to_string(value);
        return {text.begin(), text.end()};
    }

    namespace {
        // Text::Tabs' _xlen: the number of \X (grapheme clusters).
        long xlen(const sv text) {
            if (text.empty()) return 0;
            return static_cast<long>(find_all(text, re(U"\\X")).size());
        }

        constexpr long tabstop = 8;

        // Text::Tabs::expand: tabs to spaces, to the next multiple of 8 columns, line by line.
        str expand(const sv text) {
            str out;
            std::size_t start = 0;
            while (start < text.size() || (start == 0 && text.empty())) {
                std::size_t end = text.find(U'\n', start);
                end = end == sv::npos ? text.size() : end + 1;
                const sv line = text.substr(start, end - start);
                long offs = 0;
                str piece;
                std::size_t copied = 0;
                for (std::size_t i = 0; i < line.size(); ++i) {
                    if (line[i] != U'\t') continue;
                    const long pad = tabstop - (xlen(line.substr(0, i)) + offs) % tabstop;
                    offs += pad - 1;
                    piece.append(line.substr(copied, i - copied));
                    piece.append(static_cast<std::size_t>(pad), U' ');
                    copied = i + 1;
                }
                piece.append(line.substr(copied));
                out += piece;
                if (text.empty()) break;
                start = end;
            }
            return out;
        }

        // Text::Tabs::unexpand: a run of two or more spaces ending at a multiple of 8 columns
        // becomes a tab.
        str unexpand(const sv text) {
            const auto lines = split(re(U"\\n"), text, -1);
            str out;
            for (std::size_t n = 0; n < lines.size(); ++n) {
                const str line = expand(lines[n]);
                auto e = split(re(U"(\\X{8})"), line, -1);
                str lastbit;
                if (!e.empty()) {
                    lastbit = e.back();
                    e.pop_back();
                }
                if (lastbit == U"        ") lastbit = U"\t";
                str joined;
                for (auto &piece: e) {
                    subst(piece, re(U"  +$"), [](const match &) { return str{U"\t"}; });
                    joined += piece;
                }
                joined += lastbit;
                if (n) out += U'\n';
                out += joined;
            }
            return out;
        }
    }

    str text_wrap(const sv initial, const sv subsequent, const sv text, long columns) {
        const str t = expand(text);
        str lead{initial};
        long nll = columns - xlen(expand(subsequent)) - 1;
        if (nll <= 0 && !subsequent.empty()) {
            columns = xlen(expand(subsequent)) + 2;
            nll = 1;
        }
        long ll = columns - xlen(expand(initial)) - 1;
        if (ll < 0) ll = 0;
        str nl, remainder, r;
        const str brk = U"(?=\\s)\\X";
        std::size_t pos = 0;
        while (true) {
            // while ($t !~ /\G(?:$break)*\Z/gc): done when only breaks are left
            const auto done = find(t, re(U"\\G(?:" + brk + U")*\\Z"), pos);
            if (done) {
                pos = done.end();
                break;
            }
            const auto line = find(t, re(U"\\G((?:(?=[^\\n])\\X){0," + number(ll) + U"})(" + brk + U"|\\n+|\\z)", "xm"), pos);
            if (line) {
                r += unexpand(nl + lead + line[1]);
                remainder = line[2];
                pos = line.end();
            } else {
                const auto cut = find(t, re(U"\\G((?:(?=[^\\n])\\X){" + number(ll) + U"})"), pos);
                if (!cut) throw std::runtime_error("sc::pgformat: couldn't wrap a line");
                r += unexpand(nl + lead + cut[1]);
                remainder = U"\n";
                pos = cut.end();
            }
            lead = subsequent;
            ll = nll;
            nl = U"\n";
        }
        r += remainder;
        if (pos != t.size()) r += lead + t.substr(pos);
        return r;
    }
}
