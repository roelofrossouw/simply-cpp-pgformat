#pragma once
// sc::pgformat: PostgreSQL SQL and PL/pgSQL formatted, as pgFormatter 5.11's pg_format does it (a
// port of its Perl, giving the same output byte for byte):
//
//     #include <sc_pgformat.h>
//
//     sc::pgformat::options settings;
//     settings.keyword_case = sc::pgformat::letter_case::lower;
//     std::string pretty = sc::pgformat::format("select a,b from t where c=1", settings);
//
// pgFormatter is Gilles Darold's (https://github.com/darold/pgFormatter, PostgreSQL Licence), with
// code from SQL::Beautify by Jonas Kramer (Artistic License 2.0); see NOTICE.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sc::pgformat {
    // How keywords, functions or types are written.
    enum class letter_case { unchanged = 0, lower = 1, upper = 2, capitalize = 3 };

    // In a list, the comma at the end of a line (a,\nb) or at the start of the next (a\n, b).
    enum class comma_position { end, start };

    // Plain text, or HTML with <span class="..."> around keywords, functions, numbers and strings.
    enum class output_format { text, html };

    // pg_format's options, by the same names (its command-line option in brackets).
    struct options {
        long spaces = 4;                                      // indentation (-s)
        bool tabs = false;                                    // indent with a tab a level (-T)
        letter_case keyword_case = letter_case::upper;        // (-u)
        letter_case function_case = letter_case::unchanged;   // (-f)
        letter_case type_case = letter_case::lower;           // (-U)
        comma_position comma = comma_position::end;           // (-b, -e)
        bool comma_break = false;                             // a newline after each comma in INSERT (-B)
        output_format format = output_format::text;           // (-F)
        bool no_comments = false;                             // drop comments (-n)
        bool no_grouping = false;                             // a blank line between statements in a transaction (-g)
        bool numbering = false;                               // "-- Statement # n" before each statement (-N)
        bool keep_newline = false;                            // keep empty lines in PL/pgSQL (-k)
        bool no_extra_line = false;                           // no empty line at the end (-L)
        bool format_type = false;                             // another layout for some statements (-t)
        bool no_space_function = false;                       // no space between a function and ( (--no-space-function)
        bool redundant_parenthesis = false;                   // keep ((x)) in DML (--redundant-parenthesis)
        bool vertical_align = false;                          // line up CREATE TABLE columns (--vertical-align)
        long wrap_limit = 0;                                  // wrap lines longer than this (-w), 0: don't
        long wrap_after = 0;                                  // items a line in lists (-W), 0: one a line
        bool wrap_comment = false;                            // with wrap_limit, wrap comments too (-C)
        std::size_t maxlength = 0;                            // cut the input at this many characters (-m), 0: don't
        bool anonymize = false;                               // replace literals by random text (-a)
        std::string placeholder;                              // a regular expression (Perl/PCRE) for code to leave alone (-p)
        bool multiline = false;                               // the placeholder's . matches newlines too (-M)
        std::string separator;                                // dynamic code separator (-S), default '
        std::vector<std::string> extra_keywords;              // formatted as PostgreSQL keywords (--extra-keyword)
        std::vector<std::string> extra_functions;             // formatted as PostgreSQL functions (--extra-function)
        bool redshift = false;                                // Amazon Redshift's keywords too (-r, --extra-keyword redshift)
    };

    // The SQL (UTF-8) formatted. A placeholder that isn't a valid regular expression throws
    // std::invalid_argument.
    std::string format(std::string_view sql, const options &settings = {});

    // The version of pgFormatter this follows, "5.11".
    std::string_view pgformatter_version();
}
