#pragma once
// What the port of pgFormatter needs from Perl, on PCRE2's 32-bit library: text is UTF-32 (as Perl
// sees a decoded string: one element a character, so lengths and positions count characters), and
// regular expressions are compiled in UTF and Unicode-property mode, as Perl matches \w, \s, \d and
// \b in such a string. Each helper keeps the Perl behaviour pgFormatter relies on, named after it.

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sc::pgformat::detail {
    using str = std::u32string;
    using sv = std::u32string_view;

    // UTF-8 to UTF-32 and back. Malformed UTF-8 becomes U+FFFD, as Perl's :encoding(UTF-8) reads it.
    str u32(std::string_view utf8);
    std::string u8(sv text);

    // A compiled pattern. flags: any of i (caseless), s (. matches \n), x (extended), m (multi-line).
    // A pattern PCRE2 can't compile throws std::invalid_argument, as Perl dies on it.
    class regex {
    public:
        regex(sv pattern, std::string_view flags);
        ~regex();
        regex(const regex &) = delete;
        regex &operator=(const regex &) = delete;
        [[nodiscard]] void *code() const { return code_; }
        [[nodiscard]] int groups() const { return groups_; }

    private:
        void *code_ = nullptr;
        int groups_ = 0;
    };

    // The compiled pattern, from a cache (per thread): code calls this for every match it makes.
    const regex &re(sv pattern, std::string_view flags = {});

    // A match: [n] is $n ("" when it didn't take part), defined(n) tells undef apart, begin()/end()
    // are @- and @+ of the whole match.
    class match {
    public:
        match() = default;
        explicit operator bool() const { return ok_; }
        [[nodiscard]] const str &operator[](std::size_t n) const;
        [[nodiscard]] bool defined(std::size_t n) const { return n < groups_.size() && groups_[n].has_value(); }
        [[nodiscard]] std::size_t begin() const { return begin_; }
        [[nodiscard]] std::size_t end() const { return end_; }
        [[nodiscard]] std::size_t size() const { return groups_.size(); }

    private:
        friend match find(sv, const regex &, std::size_t, unsigned);
        bool ok_ = false;
        std::vector<std::optional<str>> groups_;
        std::size_t begin_ = 0, end_ = 0;
    };

    // The first match at or after offset (options: PCRE2 match options).
    match find(sv subject, const regex &pattern, std::size_t offset = 0, unsigned options = 0);
    // $s =~ /pattern/flags
    inline match find(sv subject, sv pattern, std::string_view flags = {}) { return find(subject, re(pattern, flags)); }
    inline bool test(sv subject, sv pattern, std::string_view flags = {}) { return static_cast<bool>(find(subject, pattern, flags)); }

    // A list-context m//g: every match's groups in order ($1, $2 ...), or the whole matches when
    // the pattern has no groups.
    std::vector<str> find_all(sv subject, const regex &pattern);

    using replacer = std::function<str(const match &)>;
    // s/pattern/replacement/ once: true when it matched; *matched gets the match ($1 ... afterwards).
    bool subst(str &subject, const regex &pattern, const replacer &replacement, match *matched = nullptr);
    // s/pattern/replacement/g: how many it replaced.
    std::size_t subst_all(str &subject, const regex &pattern, const replacer &replacement);
    // A replacement written as Perl writes one: $1, ${1} and the rest literal.
    replacer with(sv replacement);

    // split /pattern/, subject, limit, with Perl's rules: groups' text is returned between the
    // fields, a match of no width at the start makes no empty field, and with limit 0 the empty
    // fields at the end are dropped (a negative limit keeps them).
    std::vector<str> split(const regex &pattern, sv subject, int limit = 0);

    // Perl's lc, uc, ucfirst (Unicode simple case mapping), quotemeta, x (repetition) and truth: ""
    // and "0" are false.
    str lc(sv text);
    str uc(sv text);
    str ucfirst(sv text);
    str quotemeta(sv text);
    str repeat(sv text, long times);
    inline bool truthy(sv text) { return !(text.empty() || text == U"0"); }
    inline bool truthy(const str &text) { return truthy(sv{text}); }
    inline bool truthy(const std::optional<str> &text) { return text && truthy(sv{*text}); }
    // $a . $b for the numbers pgFormatter puts in its placeholders.
    str number(long value);

    // Text::Wrap's wrap($initial, $subsequent, $text) with $Text::Wrap::columns = columns and its
    // other settings at their defaults (as Perl 5.34 ships it, 2013.0523): words are broken at
    // white space, a word longer than a line is cut, and runs of spaces are turned into tabs
    // (unexpand).
    str text_wrap(sv initial, sv subsequent, sv text, long columns);
}
