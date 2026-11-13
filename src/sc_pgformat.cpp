#include "sc_pgformat.h"

#include "beautify.h"
#include "dicts.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>

namespace sc::pgformat {
    std::string format(const std::string_view sql, const options &settings) {
        using namespace detail;
        detail::settings s;
        s.spaces = settings.spaces;
        if (settings.tabs) {
            s.spaces = 1;
            s.space = U"\t";
        }
        s.uc_keywords = static_cast<long>(settings.keyword_case);
        s.uc_functions = static_cast<long>(settings.function_case);
        s.uc_types = static_cast<long>(settings.type_case);
        s.no_comments = settings.no_comments;
        s.no_grouping = settings.no_grouping;
        s.placeholder = u32(settings.placeholder);
        s.multiline = settings.multiline;
        s.separator = u32(settings.separator);
        s.comma = settings.comma == comma_position::start ? U"start" : U"end";
        s.comma_break = settings.comma_break;
        s.format = settings.format == output_format::html ? U"html" : U"text";
        s.format_type = settings.format_type;
        s.wrap_limit = settings.wrap_limit;
        s.wrap_after = settings.wrap_after;
        s.wrap_comment = settings.wrap_comment;
        s.numbering = settings.numbering;
        s.no_extra_line = settings.no_extra_line;
        s.keep_newline = settings.keep_newline;
        s.no_space_function = settings.no_space_function;
        s.redundant_parenthesis = settings.redundant_parenthesis;
        s.vertical_align = settings.vertical_align;

        // pgFormatter::CLI::beautify()
        str query = u32(sql);
        if (truthy(query) && settings.maxlength && query.size() > settings.maxlength) query.resize(settings.maxlength);

        beautify b(s);
        if (!settings.extra_functions.empty()) {
            std::vector<str> words;
            for (const auto &word: settings.extra_functions) words.push_back(u32(word));
            b.add_functions(words);
        }
        if (!settings.extra_keywords.empty()) {
            std::vector<str> words;
            for (const auto &word: settings.extra_keywords) words.push_back(u32(word));
            b.add_keywords(words);
        } else if (settings.redshift) {
            std::vector<str> words;
            for (const auto word: redshift_keywords()) words.push_back(u32(word));
            b.add_keywords(words);
        }
        b.query(query);
        if (settings.anonymize) b.anonymize();
        b.run();
        if (settings.wrap_limit) b.wrap_lines(settings.wrap_comment);
        return u8(b.content());
    }

    std::string_view pgformatter_version() { return "5.11"; }

    namespace {
        bool is_space(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

        std::string lower(std::string text) {
            for (auto &c: text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }
    }

    options read_config(const std::string_view text, options base) {
        // pgFormatter::CLI::get_command_line_args(): /^([^\s=]+)\s*=\s*([^\s]+)/ on lines /^[a-z]/
        std::map<std::string, std::string> cfg;
        std::size_t start = 0;
        while (start <= text.size()) {
            auto end = text.find('\n', start);
            if (end == std::string_view::npos) end = text.size();
            const auto line = text.substr(start, end - start);
            start = end + 1;
            if (line.empty() || line[0] < 'a' || line[0] > 'z') continue;
            std::size_t at = 0;
            while (at < line.size() && !is_space(line[at]) && line[at] != '=') ++at;
            const auto name = lower(std::string{line.substr(0, at)});
            while (at < line.size() && is_space(line[at])) ++at;
            if (at >= line.size() || line[at] != '=') continue;
            ++at;
            while (at < line.size() && is_space(line[at])) ++at;
            std::size_t value_end = at;
            while (value_end < line.size() && !is_space(line[value_end])) ++value_end;
            if (value_end == at) continue;
            std::string value{line.substr(at, value_end - at)};
            if (name == "comma" || name == "format") value = lower(value);
            cfg.try_emplace(name, value);
        }

        const auto given = [&](const char *name) { return cfg.contains(name); };
        const auto flag = [&](const char *name) {
            const auto found = cfg.find(name);
            return found != cfg.end() && !found->second.empty() && found->second != "0";
        };
        const auto number = [&](const char *name, const long fallback) {
            const auto found = cfg.find(name);
            if (found == cfg.end()) return fallback;
            try {
                std::size_t used = 0;
                const long value = std::stol(found->second, &used);
                if (used != found->second.size()) throw std::invalid_argument(name);
                return value;
            } catch (const std::exception &) {
                throw std::invalid_argument(std::string{"pg_format configuration: "} + name + " isn't a number: " + found->second);
            }
        };
        const auto letter = [&](const char *name, const letter_case fallback) {
            const auto found = cfg.find(name);
            if (found == cfg.end()) return fallback;
            const auto &value = found->second;
            if (value.size() != 1 || value[0] < '0' || value[0] > '3')
                throw std::invalid_argument(std::string{"pg_format configuration: "} + name + " can be only one of: 0, 1, 2, or 3");
            return static_cast<letter_case>(value[0] - '0');
        };
        const auto boolean = [&](const char *name, const bool fallback) { return given(name) ? flag(name) : fallback; };

        options o = std::move(base);
        o.spaces = number("spaces", o.spaces);
        o.keyword_case = letter("keyword-case", o.keyword_case);
        o.function_case = letter("function-case", o.function_case);
        o.type_case = letter("type-case", o.type_case);
        o.comma_break = boolean("comma-break", o.comma_break);
        o.no_grouping = boolean("nogrouping", o.no_grouping);
        o.keep_newline = boolean("keep-newline", o.keep_newline);
        o.no_extra_line = boolean("no-extra-line", o.no_extra_line);
        o.maxlength = static_cast<std::size_t>(std::max(0L, number("maxlength", static_cast<long>(o.maxlength))));
        o.multiline = boolean("multiline", o.multiline);
        o.no_comments = boolean("nocomment", o.no_comments);
        o.numbering = boolean("numbering", o.numbering);
        o.anonymize = boolean("anonymize", o.anonymize);
        if (given("placeholder")) o.placeholder = cfg["placeholder"];
        if (given("separator")) o.separator = cfg["separator"];
        o.format_type = boolean("format-type", o.format_type);
        o.wrap_limit = number("wrap-limit", o.wrap_limit);
        o.wrap_after = number("wrap-after", o.wrap_after);
        o.wrap_comment = boolean("wrap-comment", o.wrap_comment);
        o.no_space_function = boolean("no-space-function", o.no_space_function);
        o.redundant_parenthesis = boolean("redundant-parenthesis", o.redundant_parenthesis);
        o.vertical_align = boolean("vertical-align", o.vertical_align);
        o.tabs = boolean("tabs", o.tabs);
        if (flag("redshift") || (given("extra-keyword") && cfg["extra-keyword"] == "redshift")) o.redshift = true;
        if (given("comma")) {
            if (cfg["comma"] != "end" && cfg["comma"] != "start")
                throw std::invalid_argument("pg_format configuration: unknown value for comma: " + cfg["comma"]);
            o.comma = cfg["comma"] == "start" ? comma_position::start : comma_position::end;
        }
        if (flag("comma-end")) o.comma = comma_position::end;
        else if (flag("comma-start")) o.comma = comma_position::start;
        if (given("format")) {
            if (cfg["format"] != "text" && cfg["format"] != "html")
                throw std::invalid_argument("pg_format configuration: unknown output format: " + cfg["format"]);
            o.format = cfg["format"] == "html" ? output_format::html : output_format::text;
        }
        return o;
    }
}
