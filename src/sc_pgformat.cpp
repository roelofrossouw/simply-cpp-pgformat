#include "sc_pgformat.h"

#include "beautify.h"
#include "dicts.h"

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
}
