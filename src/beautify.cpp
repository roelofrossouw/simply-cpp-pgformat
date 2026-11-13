// pgFormatter::Beautify, all but beautify() itself (in beautify_run.cpp): the query's preparation,
// the content's restoration, the tokenizer, the token tests, _add_token, vertical alignment,
// anonymization and line wrapping. The order follows Beautify.pm.

#include "beautify.h"

#include "dicts.h"

#include <algorithm>
#include <stdexcept>

namespace sc::pgformat::detail {
    namespace {
        str w(const std::string_view word) { return u32(word); }

        const sv math_operators = UR"~(^(?:\+|\-|\*|\/|\%|\^|\|\/|\|\|\/|\!|\!\!|\@|\&|\||\#|\~|<<|>>|=|>=|<=)$)~";

        const char32_t *const have_from_clause[] = {U"extract", U"overlay", U"substring", U"trim"};
        const char32_t *const extract_keywords[] = {
            U"century", U"day", U"decade", U"dow", U"doy", U"epoch", U"hour", U"isodow", U"isoyear", U"microseconds",
            U"millennium", U"minute", U"month", U"quarter", U"second", U"timezone", U"timezone_minute", U"week", U"year"};

        // tokenize_sql's expression, as in Beautify.pm (flags ismx).
        const sv token_re = UR"~(
        (
		(?:\\(?:copyright|errverbose|gx|gexec|gset|gdesc|q|crosstabview|watch|\?|copy|qecho|echo|if|elif|else|endif|edit|ir|include_relative|include|warn|write|html|print|out|ef|ev|h|H|i|p|r|s|w|o|e|g|q|d(?:[aAbcCdDeEfFgilLmnoOpPrRstTuvwxy+]{0,3})?|l\+?|sf\+?|sv\+?|z|a|C|f|H|t|T|x|c|pset|connect|encoding|password|conninfo|cd|setenv|timing|prompt|reset|set|unset|lo_export|lo_import|lo_list|lo_unlink|\!))(?:$|[\n]|[\ \t](?:(?!\\(?:\\|pset|reset|connect|encoding|password|conninfo|cd|setenv|timing|prompt|set|unset|lo_export|lo_import|lo_list|lo_unlink|\!|copy|qecho|echo|edit|html|include_relative|include|print|out|warn|watch|write|q))[\ \t\S])*)        # psql meta-command
		|
		AAKEYWCONST\d+AA(?:\s+AAKEYWCONST\d+AA)+             # preserved multiple constants with newline
		|
		AAKEYWCONST\d+AA             # preserved constants
		|
		\/\/			# mysql delimiter ( $$ is handled later with PG code delimiters )
		|
		\d*\.?\d+e[-+]\d+			# scientific notation with exponents
		|
		(?:COPY\s+[^\s]+\s+\((?:.*?)\\\.)		# COPY and its content
		|
		[^\s\(,]+\%(?:ROWTYPE|TYPE)      # single line comments
		|
		(?:\s*--)[\ \t\S]*      # single line comments
		|
		(?:\-\|\-) # range operator "is adjacent to"
		|
		(?:\%[\d\-\*\$]*[sIL]) # format() placeholder
		|
		(?:<\%|\%>|<<\->|<\->>|<\->)  # pg_trgm and some geometry operators
		|
		(?:\->>|\->|<\#>|\#>>|\#>|\?\&|\?\||\?|\@\?)  # Vector and Json Operators
		|
		(?:\#<=|\#>=|\#<>|\#<|\#=) # compares tinterval and reltime
		|
		(?:>>=|<<=) # inet operators
		|
		(?:!!|\@\@\@) # deprecated factorial and full text search  operators
		|
		(?:\|\|\/|\|\/) # square root and cube root
		|
		(?:\@\-\@|\@\@|\#\#|\#\-|<<\||\|>>|\&<\||\&<|\|\&>|\&>|<\^|>\^|\?\#|\#|\?<\||\?\-\||\?\-|\?\|\||\?\||\@>|<\@|\~=)
                                 # Geometric Operators
		|
		(?:~<=~|~>=~|~>~|~<~) # string comparison for pattern matching operator families
		|
		(?:!~~|!~~\*|~~\*|~~) # LIKE operators
		|
		(?:!~\*|!~|~\*) # regular expression operators
		|
		(?:\*=|\*<>|\*<=|\*>=|\*<|\*>) # composite type comparison operators
		|
		(?:\d+e[\+\-]\d+) # signed exponents
		|
		(?:<>|<=>|>=|<=|=>|==|!=|:=|=|!|<<|>>|<|>|\|\||\||&&|&|\-|\+|\*(?!/)|/(?!\*)|\%|~|\^|\?) # operators and tests
		|
		[\[\]\(\),;.]            # punctuation (parenthesis, comma)
		|
		\"\"(?!\"")             # empty double quoted string
		|
		"[^\"\s]+"\.[^\"\s\(\)=<>!~\*&:\|\-\+\%\^\?\@\#\[\]\{\}\.,;']+ # fqdn identifier form "schema".table or "table".column
		|
		[^\"\s=<>!~\*&\(\):\|\-\+\%\^\?\@\#\[\]\{\}\.,;']+\."[^\"\s]+" # fqdn identifier form schema."table" or table."column"
		|
		"[^\"\s]+"\."[^\"\s]+" # fqdn identifier form "schema"."table" or "table"."column"
		|
		"(?>(?:(?>[^"\\]+)|""|\\.)*)+" # anything inside double quotes, ungreedy
		|
		`(?>(?:(?>[^`\\]+)|``|\\.)*)+` # anything inside backticks quotes, ungreedy
		|
		[EB]*'[^']+' # anything inside single quotes, ungreedy.
		|
		/\*[\ \t\r\n\S]*?\*/      # C style comments
		|
		(?:[\w:\@]+[\$]*[\w:\@]*(?:\.(?:\w+|\*)?)*) # words, standard named placeholders, db.table.*, db.*
		|
		(?:\$\w+\$) # text delimiter
                |
                (?: \$_\$ | \$\d+ | \${1,2} | \$\w+\$) # dollar expressions - eg $_$ $3 $$ $BODY$
                |
                (?:\r\n){2,}                      # empty line Windows
                |
                \n{2,}                      # empty line Unix
                |
                \r{2,}                      # empty line Mac
                |
                [\t\ ]+                 # any kind of white spaces
		|
		[^\s\*\/\-\\;:,]+                 # anything else
        )
    )~";

        template<typename List>
        bool in_list(const List &list, const sv word) {
            return std::any_of(std::begin(list), std::end(list), [&](const char32_t *item) { return word == item; });
        }
    }

    // new() and set_defaults()
    beautify::beautify(const settings &options) : o(options) {
        for (const auto word: pg_keywords()) keywords.push_back(w(word));
        for (const auto word: pg_types()) types.push_back(w(word));
        for (const auto word: pg_functions()) {
            functions.push_back(w(word));
            pg_function_set.insert(w(word));
        }
        for (const auto &[symbol, html]: detail::symbols()) {
            symbols.emplace_back(w(symbol), w(html));
            symbol_keys.insert(w(symbol));
            symbol_keys_and_values.insert(w(symbol));
            symbol_keys_and_values.insert(w(html));
        }
        keyword_set.insert(keywords.begin(), keywords.end());
        type_set.insert(types.begin(), types.end());
        refresh_functions_re();
        if (o.spaces == 0) break_ = U" ";
        o.comma = lc(o.comma) != U"start" ? str{U"end"} : lc(o.comma);
    }

    void beautify::refresh_functions_re() {
        str joined;
        for (std::size_t i = 0; i < functions.size(); ++i) {
            if (i) joined += U'|';
            joined += functions[i];
        }
        functions_re = U"\\b[\\.]*(" + joined + U")$";
    }

    void beautify::add_keywords(const std::vector<str> &words) {
        for (const auto &word: words) {
            keywords.push_back(word);
            keyword_set.insert(word);
        }
    }

    void beautify::add_functions(const std::vector<str> &words) {
        functions.insert(functions.end(), words.begin(), words.end());
        refresh_functions_re();
    }

    str beautify::query(const opt &new_value) {
        if (new_value) query_ = *new_value;
        str &q = query_;
        idx_code = 0;

        // Replace any COMMENT constant between single quote
        match m;
        while (subst(q, re(UR"~(IS\s+([EU]*'(?:[^;]*)')\s*;)~", "is"),
                     [&](const match &) { return U"IS TEXTVALUE" + number(idx_code) + U"E;"; }, &m)) {
            str &code = dynamic_code[number(idx_code)];
            code = m[1];
            subst_all(code, re(UR"~(([\n\r])\s+([EU]*'(?:[^']*)'))~", "sei"),
                      [&](const match &x) { return x[1] + repeat(o.space, o.spaces) + x[2]; });
            ++idx_code;
        }

        // Replace any \\ by BSLHPGF
        subst_all(q, re(UR"~(\\\\)~", "s"), with(U"BSLHPGF"));
        // Replace any \' by PGFBSLHQ
        subst_all(q, re(UR"~(\\')~", "s"), with(U"PGFBSLHQ"));
        // Replace any '' by PGFESCQ1
        while (subst(q, re(UR"~(([^'])''([^']))~", "s"), with(U"$1PGFESCQ1$2"))) {}
        // Replace any '''' by PGFESCQ1PGFESCQ1
        while (subst(q, re(UR"~(([^'])''''([^']))~", "s"), with(U"$1PGFESCQ1PGFESCQ1$2"))) {}
        // Replace any '...''' by '.*PGFESCQ1'
        while (subst(q, re(UR"~(([^']'[^']+)''('[^']))~", "s"), with(U"$1PGFESCQ1$2"))) {}
        // Replace any '''...' by 'PGFESCQ1.*'
        while (subst(q, re(UR"~(([^']')''([^']+'[^']))~", "s"), with(U"$1PGFESCQ1$2"))) {}
        // Replace any multiline '''...''' by 'PGFESCQ1...PGFESCQ1'
        subst_all(q, re(UR"~(([^']')''([^']*)''('[^']|$))~", "s"), with(U"$1PGFESCQ1$2PGFESCQ1$3"));
        // Replace any "" by PGFESCQ2
        while (subst(q, re(UR"~(([^"])""([^"]))~", "s"), with(U"$1PGFESCQ2$2"))) {}

        // Replace aliases using double quote
        long j = 0;
        while (subst(q, re(UR"~((\s+AS\s*)("+[^"]+"+))~", "is"), [&](const match &x) { return x[1] + U"PGFALIAS" + number(j); }, &m)) {
            alias_constant[number(j)] = m[2];
            ++j;
        }

        // Escape quote in comments
        while (subst(q, re(UR"~((\/\*(?:.*)?)'((?:.*)?\*\/))~", "s"), with(U"$1SNGLQUOTE$2"))) {}

        // replace all constant between quote
        j = 0;
        while (subst(q, re(UR"~(('[^'\n\r]+'))~", "s"), [&](const match &) { return U"AAKEYWCONST" + number(j) + U"AA"; }, &m)) {
            keyword_constant[number(j)] = m[1];
            ++j;
        }

        // Restore quote in comments
        subst_all(q, re(U"SNGLQUOTE", "s"), with(U"'"));
        for (auto &[k, value]: keyword_constant) subst_all(value, re(U"SNGLQUOTE", "s"), with(U"'"));

        // Fix false positive generated by code above.
        while (subst(q, re(UR"~((\s+AS\s+)AAKEYWCONST(\d+)AA)~", "is"), [&](const match &x) {
            const auto found = keyword_constant.find(x[2]);
            return x[1] + (found != keyword_constant.end() ? found->second : str{});
        }, &m)) {
            keyword_constant.erase(m[2]);
        }

        // Hide content of format() function when the code separator is not a single quote
        long i = 0;
        while (subst(q, re(UR"~(\bformat\((\$(?:.*)?\$\s*)([,\)]))~", "i"),
                     [&](const match &x) { return U"format(CODEPARTB" + number(i) + U"CODEPARTB" + x[2]; }, &m)) {
            placeholder_values.push_back(m[1]);
            ++i;
        }
        auto temp_content = split(re(UR"~((CREATE(?:\s+OR\s+REPLACE)?\s+(?:FUNCTION|PROCEDURE)\s+))~", "i"), q);
        if (temp_content.size() > 1) {
            for (auto &part: temp_content) {
                if (test(part, U"^CREATE", "i") || part.empty()) continue;

                // Replace single quote code delimiter into $PGFDLM$
                if (!subst(part, re(UR"~((\s+AS\s+)'(\s+.*?;\s*)')~", "is"), with(U"$1$PGFDLM$$2$PGFDLM$")))
                    subst(part, re(UR"~((\s+AS\s+)'(\s+.*?END[;]*\s*)')~", "is"), with(U"$1$PGFDLM$$2$PGFDLM$"));

                // Remove any call too CREATE/DROP LANGUAGE to not break search of function code separator
                subst(part, re(UR"~((CREATE|DROP)\s+LANGUAGE\s+[^;]+;.*)~", "is"), with(U""));
                // Fix case where code separator with $ is associated to begin/end keywords
                subst_all(part, re(UR"~(([^\s]+\$)(BEGIN\s))~", "igs"), with(U"$1 $2"));
                subst_all(part, re(UR"~((\sEND)(\$[^\s]+))~", "igs"), with(U"$1 $2"));
                subst(part, re(UR"~((CREATE|DROP)\s+LANGUAGE\s+[^;]+;.*)~", "is"), with(U""));
                str fctname;
                if (const auto f = find(part, UR"~(^([^\s\(]+))~")) fctname = lc(f[1]);
                if (!truthy(fctname)) continue;
                str language = U"sql";
                if (const auto l = find(part, UR"~(\s+LANGUAGE\s+[']*([^'\s;]+)[']*)~", "is")) {
                    language = lc(l[1]);
                    if (const auto c = find(language, U"AAKEYWCONST(\\d+)AA", "i")) {
                        const auto found = keyword_constant.find(c[1]);
                        language = lc(found != keyword_constant.end() ? found->second : str{});
                        subst_all(language, re(U"'"), with(U""));
                    }
                }

                if (test(language, U"^internal$", "i")) {
                    if (subst(part, re(UR"~(AS ('[^\']+'))~", "is"), [&](const match &) { return U"AS CODEPARTB" + number(i) + U"CODEPARTB"; }, &m)) {
                        placeholder_values.push_back(m[1]);
                        ++i;
                    }
                }
                // C function language with AS obj_file, link_symbol
                else if (test(language, U"^c$", "i")) {
                    if (subst(part, re(UR"~(AS ('[^\']+')\s*,\s*('[^\']+'))~", "is"),
                              [&](const match &) { return U"AS CODEPARTB" + number(i) + U"CODEPARTB"; }, &m)) {
                        placeholder_values.push_back(m[1] + U", " + m[2]);
                        ++i;
                    }
                }
                // if the function language is not SQL or PLPGSQL
                else if (!test(language, U"^(?:plpg)?sql$")) {
                    // Try to find the code separator
                    str tmp_str = part;
                    while (subst(tmp_str, re(UR"~(\s+AS\s+([^\s]+)\s+)~", "is"), with(U""), &m)) {
                        str code_sep = quotemeta(m[1]);
                        for (const auto &k: keywords)
                            if (subst(code_sep, re(U"\\b" + k + U"$", "i"), with(U""))) break;
                        if (!truthy(code_sep)) continue;
                        if (test(tmp_str, U"\\s+" + code_sep + U"[\\s;]+")) {
                            match c;
                            while (subst(part, re(U"(" + code_sep + U"(?:.+?)" + code_sep + U")", "s"),
                                         [&](const match &) { return U"CODEPART" + number(i) + U"CODEPART"; }, &c)) {
                                placeholder_values.push_back(c[1]);
                                ++i;
                            }
                            break;
                        }
                    }
                }
            }
        }
        str joined;
        for (const auto &part: temp_content) joined += part;
        q = joined;

        // replace all inline dollar-quoted constants, but leave untouched the ones that are not real
        // string constants: PL/pgSQL code bodies introduced by AS or DO, and MySQL DELIMITER $$.
        subst_all(q, re(UR"~(
                (\bAS\s+ | \bDO\s+(?:LANGUAGE\s+\w+\s+)? | \bDELIMITER[ \t]+)?   # $1: skip prefix, kept verbatim
                (\$\$[ ]*\S[^\n\r]*?\$\$)                                        # $2: inline $$...$$
        )~", "isx"), [&](const match &x) {
            if (x.defined(1)) return x[1] + x[2];
            keyword_constant[number(j)] = x[2];
            str placeholder = U"AAKEYWCONST" + number(j) + U"AA";
            ++j;
            return placeholder;
        });

        // Store values of code that must not be changed following the given placeholder
        if (truthy(o.placeholder)) {
            const auto &pattern = re(U"(" + o.placeholder + U")", o.multiline ? "s" : "");
            while (subst(q, pattern, [&](const match &) { return U"PLACEHOLDER" + number(i) + U"PLACEHOLDER"; }, &m)) {
                placeholder_values.push_back(m[1]);
                ++i;
            }
        }

        // Replace dynamic code with placeholder
        remove_dynamic_code(q, o.separator);
        // Replace operator with placeholder
        quote_operator(q);
        // Replace comment with not quote delimiter with placeholder
        quote_comment_stmt(q);
        return q;
    }

    str beautify::content(const opt &new_value) {
        if (new_value) content_ = *new_value;
        str &c = content_;
        subst_all(c, re(UR"~(\(\s+\()~", "s"), with(U"(("));

        // Replace placeholders with their original dynamic code
        restore_dynamic_code(c);
        // Replace placeholders with their original operator
        restore_operator(c);
        // Replace placeholders with their original string
        restore_comment_stmt(c);

        // Replace placeholders by their original values
        if (!placeholder_values.empty()) {
            const auto value = [&](const match &x) {
                const auto n = std::stoul(std::string(x[1].begin(), x[1].end()));
                return n < placeholder_values.size() ? placeholder_values[n] : str{};
            };
            subst_all(c, re(U"PLACEHOLDER(\\d+)PLACEHOLDER", "igs"), value);
            subst_all(c, re(U"CODEPART[B]*(\\d+)CODEPART[B]*", "igs"), value);
        }

        subst_all(c, re(U"PGFALIAS(\\d+)", "gs"), [&](const match &x) {
            const auto found = alias_constant.find(x[1]);
            return found != alias_constant.end() ? found->second : str{};
        });

        match m;
        while (subst(c, re(U"AAKEYWCONST(\\d+)AA", "s"), [&](const match &x) {
            const auto found = keyword_constant.find(x[1]);
            return found != keyword_constant.end() ? found->second : str{};
        }, &m)) {
            keyword_constant.erase(m[1]);
        }

        subst_all(c, re(U"BSLHPGF"), with(UR"~(\\)~"));
        subst_all(c, re(U"PGFBSLHQ"), with(UR"~(\')~"));
        subst_all(c, re(U"\\$PGFDLM\\$"), with(U"'"));
        subst_all(c, re(U"PGFESCQ1"), with(U"''"));
        subst_all(c, re(U"PGFESCQ2"), with(U"\"\""));

        if (o.vertical_align && o.format == U"text") c = align_create_table_columns(c);
        return c;
    }

    str beautify::highlight_code(str token, const opt &last_token, const opt &next_token) {
        const str last = last_token.value_or(str{});
        const str next = next_token.value_or(str{});

        // Colorize operators
        for (const auto &[k, v]: symbols)
            if (token == k) return U"<span class=\"sy0\">" + v + U"</span>";

        // lowercase/uppercase keywords taking care of function with same name
        if (is_keyword(token, next, last) && (!is_function(token, last, next) || next != U"(")) {
            const char32_t *cls = o.uc_keywords == 1 ? U"kw1_l" : o.uc_keywords == 2 ? U"kw1_u" : o.uc_keywords == 3 ? U"kw1_c" : U"kw1";
            return U"<span class=\"" + str{cls} + U"\">" + token + U"</span>";
        }

        // lowercase/uppercase known functions or words followed by an open parenthesis if the
        // token is not a keyword, an open parenthesis or a comment
        if ((is_function(token, last, next) && next == U"(") ||
            (!is_keyword(token, next, last) && next != U"(" && token != U"(" && !is_comment(token))) {
            const char32_t *cls = o.uc_functions == 1 ? U"kw2_l" : o.uc_functions == 2 ? U"kw2_u" : o.uc_functions == 3 ? U"kw2_c" : U"kw2";
            return U"<span class=\"" + str{cls} + U"\">" + token + U"</span>";
        }

        // Colorize STDIN/STDOUT in COPY statement
        for (const auto word: copy_keywords())
            if (uc(token) == uc(w(word))) {
                const char32_t *cls = o.uc_keywords == 1 ? U"kw3_!" : o.uc_keywords == 2 ? U"kw3_u" : o.uc_keywords == 3 ? U"kw3_c" : U"kw3";
                return U"<span class=\"" + str{cls} + U"\">" + token + U"</span>";
            }

        // Colorize parenthesis
        for (const auto bracket: brackets())
            if (token == w(bracket)) return U"<span class=\"br0\">" + token + U"</span>";

        // Colorize comment
        if (is_comment(token)) return U"<span class=\"br1\">" + token + U"</span>";

        // Colorize numbers
        subst_all(token, re(UR"~(\b(\d+)\b)~", "igs"), with(U"<span class=\"nu0\">$1</span>"));
        // Colorize string
        subst_all(token, re(UR"~(('.*?(?<!\\)'))~", "gs"), with(U"<span class=\"st0\">$1</span>"));
        subst_all(token, re(UR"~((`[^`]*`))~", "gs"), with(U"<span class=\"st0\">$1</span>"));
        return token;
    }

    std::vector<str> beautify::tokenize_sql(const opt &query_arg) {
        str q = query_arg.value_or(str{});
        if (!truthy(q)) q = query_;
        // just in case it has not been called in the main script
        if (!truthy(q)) q = query();

        const auto &pattern = re(token_re, "ismx");
        std::vector<str> list;
        for (auto &t: find_all(q, pattern))
            if (o.keep_newline ? test(t, UR"~((?:\S|^[\r\n]+$))~") : test(t, U"\\S")) list.push_back(std::move(t));

        // fix := operator that can not be found when attached to the variable name
        for (std::size_t i = 0; i + 1 < list.size(); ++i) {
            if (test(list[i], U":$") && test(list[i + 1], U"^=")) {
                subst(list[i], re(U":$"), with(U""));
                list[i + 1] = U":" + list[i + 1];
            }
        }

        // Revert position when a comment is before a comma
        if (o.comma == U"end") {
            for (long i = 0; i < static_cast<long>(list.size()) - 2; ++i) {
                if (list[i + 1] == U"," && is_comment(list[i])) {
                    list[i + 1] = list[i];
                    list[i] = U",";
                }
            }
        }

        // Fix token split of negative numbers
        if (list.size() > 3) {
            for (std::size_t i = 2; i < list.size(); ++i) {
                if (test(list[i], UR"~(^[\d\.]+$)~") && test(list[i - 1], UR"~(^[\+\-]$)~") &&
                    (test(list[i - 2], math_operators) || test(list[i - 2], UR"~(^(?:,|\(|\[)$)~") || is_keyword(list[i - 2]))) {
                    list[i] = list[i - 1] + list[i];
                    list[i - 1].clear();
                } else if (test(list[i], UR"~(^[\d\.]+[^\d\.]+$)~") && test(list[i - 1], UR"~(^\%$)~") &&
                           (test(list[i - 2], math_operators) || test(list[i - 2], UR"~(^(?:,|\(|\[)$)~") ||
                            is_keyword(list[i - 2]) || is_type(list[i - 2]))) {
                    list[i] = list[i - 1] + list[i];
                    list[i - 1].clear();
                } else if (test(list[i - 1], UR"~(\$\w+$)~") && list[i] == U"$") {
                    match m;
                    subst(list[i - 1], re(UR"~((\$\w+)$)~"), with(U""), &m);
                    list[i] = m[1] + list[i];
                }
            }
        }
        std::erase_if(list, [](const str &t) { return test(t, U"^$"); });
        return list;
    }

    std::optional<beautify::column> beautify::parse_create_table_column(const str &line) {
        if (!test(line, U"\\S") || test(line, U"[\\r\\n]")) return std::nullopt;

        column c;
        c.indent = find(line, UR"~(^(\s*))~")[1];
        str definition = line;
        subst_all(definition, re(UR"~(^\s+|\s+$)~"), with(U""));

        auto list = tokenize_sql(definition);

        // A trailing comment belongs to the complete column definition rather than to its data
        // type or constraints.
        if (!list.empty() && is_comment(list.back())) {
            c.comment = list.back();
            list.pop_back();
            subst(c.comment, re(UR"~(^\s+)~"), with(U""));
        }
        // Comments embedded in the middle of a definition remain unsupported.
        for (const auto &t: list)
            if (is_comment(t)) return std::nullopt;

        if (!list.empty() && list.back() == U",") {
            list.pop_back();
            c.comma = U",";
        }
        if (list.size() < 2) return std::nullopt;

        c.name = list.front();
        list.erase(list.begin());

        // These tokens start table-level definitions rather than column definitions.
        static const char32_t *const table_constraint[] = {U"CHECK", U"CONSTRAINT", U"EXCLUDE", U"FOREIGN", U"LIKE", U"PRIMARY", U"UNIQUE"};
        if (!test(c.name, U"^\"") && in_list(table_constraint, uc(c.name))) return std::nullopt;
        if (test(c.name, UR"~(^(?:[\[\](),;])$)~")) return std::nullopt;

        // A column's data type ends at the first top-level column constraint.
        static const char32_t *const column_constraint[] = {U"CHECK", U"COLLATE", U"COMPRESSION", U"CONSTRAINT", U"DEFAULT",
                                                            U"GENERATED", U"IDENTITY", U"NOT", U"NULL", U"PRIMARY",
                                                            U"REFERENCES", U"STORAGE", U"UNIQUE"};
        long parenthesis_depth = 0, bracket_depth = 0;
        std::size_t constraint_index = list.size();
        for (std::size_t index = 0; index < list.size(); ++index) {
            const auto &t = list[index];
            if (parenthesis_depth == 0 && bracket_depth == 0 && in_list(column_constraint, uc(t))) {
                constraint_index = index;
                break;
            }
            if (t == U"(") ++parenthesis_depth;
            if (t == U")" && parenthesis_depth > 0) --parenthesis_depth;
            if (t == U"[") ++bracket_depth;
            if (t == U"]" && bracket_depth > 0) --bracket_depth;
        }
        c.declaration_tokens.assign(list.begin(), list.begin() + static_cast<long>(constraint_index));
        c.remainder_tokens.assign(list.begin() + static_cast<long>(constraint_index), list.end());
        if (c.declaration_tokens.empty()) return std::nullopt;

        // Keep an immediately following PRIMARY KEY or UNIQUE with the declaration.
        auto &rest = c.remainder_tokens;
        if (rest.size() >= 2 && uc(rest[0]) == U"PRIMARY" && uc(rest[1]) == U"KEY") {
            c.declaration_tokens.insert(c.declaration_tokens.end(), rest.begin(), rest.begin() + 2);
            rest.erase(rest.begin(), rest.begin() + 2);
        } else if (!rest.empty() && uc(rest[0]) == U"UNIQUE") {
            c.declaration_tokens.push_back(rest.front());
            rest.erase(rest.begin());
        }
        return c;
    }

    str beautify::render_sql_tokens(const std::vector<str> &list) {
        if (list.empty()) return {};
        static const char32_t *const space_before_parenthesis[] = {U"AS", U"CHECK", U"DEFAULT", U"IN", U"REFERENCES"};
        str rendered;
        for (std::size_t index = 0; index < list.size(); ++index) {
            const auto &t = list[index];
            if (index == 0) {
                rendered = t;
                continue;
            }
            const auto &previous = list[index - 1];
            // Closing punctuation, casts, and array suffixes attach directly to the preceding token.
            if (test(t, UR"~(^(?:\)|\]|,|;)$)~") || test(t, U"^::") || previous == U"(" || previous == U"[" || t == U"[") {
                rendered += t;
                continue;
            }
            if (t == U"(") {
                const bool force_space = in_list(space_before_parenthesis, uc(previous)) || (index > 1 && uc(list[index - 2]) == U"REFERENCES");
                const bool attach = o.no_space_function || is_function(previous) || is_type(previous);
                rendered += (force_space || !attach ? U" " : U"") + t;
                continue;
            }
            rendered += U" " + t;
        }
        return rendered;
    }

    str beautify::pad_right(const str &value, const long width) {
        const long padding = width - static_cast<long>(value.size());
        if (padding <= 0) return value;
        return value + str(static_cast<std::size_t>(padding), U' ');
    }

    void beautify::align_create_table_column_keyword(std::vector<column> &rows, const sv keyword) {
        if (rows.empty() || keyword.empty()) return;
        const str keyword_upper = uc(keyword);
        long prefix_width = 0;
        struct found {
            column *row;
            str prefix, clause;
        };
        std::vector<found> matches;
        for (auto &row: rows) {
            if (row.original) continue;
            const auto &list = row.remainder_tokens;
            if (list.empty()) continue;
            long parenthesis_depth = 0, bracket_depth = 0;
            std::optional<std::size_t> keyword_index;
            for (std::size_t index = 0; index < list.size(); ++index) {
                const auto &t = list[index];
                if (parenthesis_depth == 0 && bracket_depth == 0 && uc(t) == keyword_upper) {
                    keyword_index = index;
                    break;
                }
                if (t == U"(") ++parenthesis_depth;
                if (t == U")" && parenthesis_depth > 0) --parenthesis_depth;
                if (t == U"[") ++bracket_depth;
                if (t == U"]" && bracket_depth > 0) --bracket_depth;
            }
            if (!keyword_index) continue;
            const std::vector<str> prefix_tokens(list.begin(), list.begin() + static_cast<long>(*keyword_index));
            const std::vector<str> clause_tokens(list.begin() + static_cast<long>(*keyword_index), list.end());
            str prefix = render_sql_tokens(prefix_tokens);
            str clause = render_sql_tokens(clause_tokens);
            prefix_width = std::max(prefix_width, static_cast<long>(prefix.size()));
            matches.push_back({&row, std::move(prefix), std::move(clause)});
        }
        if (matches.size() < 2) return;
        for (auto &m: matches) {
            if (!m.prefix.empty()) m.row->remainder = pad_right(m.prefix, prefix_width) + U" " + m.clause;
            else m.row->remainder = str(static_cast<std::size_t>(prefix_width + 1), U' ') + m.clause;
        }
    }

    long beautify::parenthesis_delta(const str &line) {
        if (line.empty()) return 0;
        long delta = 0;
        for (const auto &t: tokenize_sql(line)) {
            if (t == U"(") ++delta;
            if (t == U")") --delta;
        }
        return delta;
    }

    void beautify::create_table_start(const str &line, bool &is_create_table, bool &has_opening, long &opening_depth) {
        is_create_table = has_opening = false;
        opening_depth = 0;
        if (!test(line, UR"~(^\s*CREATE\b)~", "i")) return;
        const auto list = tokenize_sql(line);
        if (list.empty() || uc(list[0]) != U"CREATE") return;
        std::optional<std::size_t> table_index;
        for (std::size_t index = 1; index < list.size(); ++index) {
            const str t = uc(list[index]);
            if (t == U"TABLE") {
                table_index = index;
                break;
            }
            if (!test(t, U"^(?:GLOBAL|LOCAL|TEMP|TEMPORARY|UNLOGGED)$")) return;
        }
        if (!table_index) return;
        bool opening = false;
        for (std::size_t index = *table_index + 1; index < list.size(); ++index) {
            if (test(uc(list[index]), U"^(?:AS|OF|PARTITION)$")) return;
            if (list[index] == U"(") {
                opening = true;
                break;
            }
        }
        is_create_table = true;
        has_opening = opening;
        opening_depth = opening ? parenthesis_delta(line) : 0;
    }

    std::vector<str> beautify::align_create_table_body(const std::vector<body_line> &body) {
        std::vector<str> result;
        for (const auto &entry: body) result.push_back(entry.line);
        std::vector<std::size_t> candidate_indices;
        std::vector<str> candidate_lines;
        for (std::size_t index = 0; index < body.size(); ++index) {
            if (body[index].depth_before != 1 || body[index].depth_after != 1) continue;
            candidate_indices.push_back(index);
            candidate_lines.push_back(body[index].line);
        }
        if (candidate_lines.empty()) return result;
        const auto aligned = align_create_table_column_group(candidate_lines);
        for (std::size_t index = 0; index < candidate_indices.size(); ++index) result[candidate_indices[index]] = aligned[index];
        return result;
    }

    str beautify::align_create_table_columns(const str &text) {
        if (text.empty()) return text;
        const str newline = text.find(U"\r\n") != str::npos ? U"\r\n" : U"\n";
        const auto lines = split(re(quotemeta(newline)), text, -1);
        std::vector<str> output;
        std::vector<body_line> body;
        long depth = 0;
        bool inside_table = false, pending_table = false;

        for (const auto &line: lines) {
            if (inside_table) {
                const long depth_before = depth;
                const long depth_after = depth + parenthesis_delta(line);
                if (depth_after <= 0) {
                    for (auto &aligned: align_create_table_body(body)) output.push_back(std::move(aligned));
                    output.push_back(line);
                    body.clear();
                    depth = 0;
                    inside_table = false;
                    continue;
                }
                body.push_back({line, depth_before, depth_after});
                depth = depth_after;
                continue;
            }
            if (pending_table) {
                const auto list = tokenize_sql(line);
                const str first = list.empty() ? str{} : list[0];
                output.push_back(line);
                if (first == U"(") {
                    depth = parenthesis_delta(line);
                    inside_table = depth > 0;
                    pending_table = false;
                } else if (test(line, U"\\S") && !(!list.empty() && is_comment(list[0]))) {
                    pending_table = false;
                }
                continue;
            }
            bool is_create_table = false, has_opening = false;
            long opening_depth = 0;
            create_table_start(line, is_create_table, has_opening, opening_depth);
            output.push_back(line);
            if (!is_create_table) continue;
            if (has_opening) {
                depth = opening_depth;
                inside_table = depth > 0;
            } else {
                pending_table = true;
            }
        }
        // Incomplete input is still valid formatter input: keep the collected lines.
        for (const auto &entry: body) output.push_back(entry.line);

        str joined;
        for (std::size_t i = 0; i < output.size(); ++i) {
            if (i) joined += newline;
            joined += output[i];
        }
        return joined;
    }

    void beautify::align_create_table_column_comments(std::vector<column> &rows) {
        if (rows.empty()) return;
        long sql_width = 0;
        bool has_comment = false;
        for (const auto &row: rows) {
            if (row.original) continue;
            sql_width = std::max(sql_width, static_cast<long>(row.rendered.size()));
            if (!row.comment.empty()) has_comment = true;
        }
        if (!has_comment) return;
        for (auto &row: rows) {
            if (row.original || row.comment.empty()) continue;
            row.rendered = pad_right(row.rendered, sql_width) + U" " + row.comment;
        }
    }

    std::vector<str> beautify::align_create_table_column_group(const std::vector<str> &lines) {
        if (lines.empty()) return {};
        std::vector<column> rows;
        long name_width = 0, declaration_width = 0, alignable_count = 0;
        for (const auto &line: lines) {
            auto c = parse_create_table_column(line);
            if (!c) {
                column original;
                original.original = true;
                original.line = line;
                rows.push_back(std::move(original));
                continue;
            }
            c->declaration = render_sql_tokens(c->declaration_tokens);
            c->remainder = render_sql_tokens(c->remainder_tokens);
            name_width = std::max(name_width, static_cast<long>(c->name.size()));
            declaration_width = std::max(declaration_width, static_cast<long>(c->declaration.size()));
            rows.push_back(std::move(*c));
            ++alignable_count;
        }
        // A single definition has nothing to align with.
        if (alignable_count < 2) return lines;

        align_create_table_column_keyword(rows, U"DEFAULT");
        for (auto &row: rows) {
            if (row.original) continue;
            str line = row.indent + pad_right(row.name, name_width) + U" " + row.declaration;
            if (!row.remainder.empty())
                line += str(static_cast<std::size_t>(std::max(0L, declaration_width - static_cast<long>(row.declaration.size()))), U' ') + U" " + row.remainder;
            row.rendered = line + row.comma;
        }
        align_create_table_column_comments(rows);
        std::vector<str> aligned;
        for (const auto &row: rows) aligned.push_back(row.original ? row.line : row.rendered);
        return aligned;
    }

    long beautify::pop_level() {
        if (level_stack.empty()) return 0;
        const long value = level_stack.back();
        level_stack.pop_back();
        return value;
    }

    void beautify::reset_level() {
        level_stack.clear();
        level = 0;
        if (o.spaces == 0) break_ = U" ";
    }

    void beautify::set_level(const std::optional<long> &position) {
        if (!position) return;
        level = *position >= 0 ? *position : 0;
    }

    void beautify::push_level(const long position) { level_stack.push_back(position >= 0 ? position : 0); }

    // _add_token: add a token to the beautified string.
    void beautify::add_token(str token, const opt &last_token) {
        const str last = last_token.value_or(str{});

        if (o.keep_newline && is_in_block >= 0 && test(token, UR"~(^[\r\n]+$)~", "s") && last_token && last == U";") {
            subst(token, re(UR"~(^[\r\n]+$)~", "s"), with(U"\n"));
            subst(content_, re(UR"~(\s+$)~", "s"), with(U"\n"));
            if (!test(content_, UR"~([\n]{2,}$)~", "s")) content_ += token;
            return;
        }

        const bool last_is_dot = last_token && last == U".";
        const str sp = indent();
        if (!is_punctuation(token) && !last_is_dot) {
            if ((!last_token || last != U"(") && token != U")" && !test(token, U"^::")) {
                if (token != U")" && last_token && !test(last, U"::$") && last != U"[" && (token != U"(" || !is_type(last)) &&
                    (token != U"(" || !is_function(last) || is_in_type)) {
                    if ((!test(token, U"PGFESCQ[12]") || !test(last, U"'$")) && (!test(last, U"PGFESCQ[12]") || !test(token, U"^'"))) {
                        if (!test(token, UR"~(^['"].*['"]$)~") || last != U":") {
                            if (test(token, UR"~(AAKEYWCONST\d+AA\s+AAKEYWCONST\d+AA)~")) {
                                subst_all(token, re(U"(AAKEYWCONST\\d+AA)", "gs"), [&](const match &x) { return sp + x[1]; });
                            } else {
                                if (!o.no_space_function || token != U"(" || (!is_in_drop_function && !is_in_create_function && !is_in_trigger))
                                    content_ += sp;
                                if (o.no_space_function && token == U"(" && (!is_keyword(last, token, std::nullopt) || is_type(last)))
                                    subst(content_, re(sp + U"$", "s"), with(U""));
                            }
                        }
                    }
                } else if (!last_token && truthy(token)) {
                    content_ += sp;
                } else if (token == U"(" && is_in_create == 2 && !test(content_, sp + U"$")) {
                    content_ += sp;
                } else if (last_token && is_comment(last)) {
                    content_ += sp;
                }
            } else if (last_token && last == U"(" && token != U")" && !test(token, U"^::") && !o.wrap_after && is_in_with == 1) {
                content_ += sp;
            } else if (is_in_create == 2 && last_token) {
                if (last != U"::" && !is_in_partition && !is_in_policy && !is_in_trigger && !is_in_aggregate &&
                    (last != U"(" || !is_in_index)) {
                    if (last != U"(" || !is_in_function) content_ += sp;
                    if (test(token, U"^::")) subst(content_, re(sp + U"$", "s"), with(U""));
                }
            } else if (last_token && (!is_in_operator || !is_in_alter)) {
                if (last == U"(" && (is_in_type || (is_in_operator && !is_type(token, last, next_token())))) {
                    if (!is_in_type || !test(token, UR"~(^\d+$)~")) content_ += sp;
                } else if (o.comma_break && current_sql_stmt == U"INSERT") {
                    content_ += sp;
                }
            } else if (token == U")" && is_in_block >= 0 && is_in_create && !is_in_create_table) {
                content_ += sp;
            }

            if (is_comment(token)) {
                auto lines = split(re(U"\\n"), token);
                for (std::size_t i = 1; i < lines.size(); ++i) {
                    if (test(lines[i], UR"~(^\s*\*)~")) subst(lines[i], re(UR"~(^\s*\*)~"), [&](const match &) { return sp + U" *"; });
                    else if (test(lines[i], UR"~(^\s+[^\*])~")) subst(lines[i], re(UR"~(^\s+)~"), [&](const match &) { return sp + U" "; });
                }
                token.clear();
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    if (i) token += U'\n';
                    token += lines[i];
                }
            } else if (is_in_function && fct_code_delimiter == U"'") {
                subst_all(token, re(U"\\n", "gs"), [&](const match &) { return U"\n" + sp; });
            }
        }

        // $self->_next_token || '': a "0" counts as nothing too
        str next = truthy(next_token()) ? *next_token() : str{};
        std::vector<str> cast, next_cast;

        // Be sure that we not going to modify a constant
        if (is_in_create < 2 && !test(token, U"^[E]*'.*'$")) {
            cast = split(re(U"::"), token, -1);
            if (!cast.empty()) {
                token = cast.front();
                cast.erase(cast.begin());
            }
            next_cast = split(re(U"::"), next);
            if (!next_cast.empty()) next = next_cast.front();
        }

        // lowercase/uppercase keywords taking care of function with same name
        if (is_keyword(token, next, last_token) &&
            (!is_type(next) || is_in_create < 2 || is_in_cast || (is_in_create == 2 && test(token, U"^(WITH|WITHOUT)$", "i")) ||
             is_in_create_function || uc(token) == U"AS") &&
            (next != U"(" || (last_token && test(last, U"^(CREATE|ALTER)$", "i")) || !is_function(token))) {
            // Be sure that we are not formating with time zone
            if (uc(token) != U"WITH" || !test(next, U"^(time|timestamp)$", "i")) {
                if (o.uc_keywords == 1) token = lc(token);
                if (o.uc_keywords == 2) token = uc(token);
                if (o.uc_keywords == 3) token = ucfirst(lc(token));
            }
        } else {
            // lowercase/uppercase known functions or words followed by an open parenthesis if the
            // token is not a keyword, an open parenthesis or a comment
            str fct = is_function(token, last_token, next).value_or(str{});
            if (truthy(fct) && ((next == U"(" && last_token && uc(last) != U"CREATE") ||
                                (!is_keyword(token, next, last_token) && next != U"(" && token != U"(" && !is_comment(token)))) {
                if (o.uc_functions == 1) subst(token, re(fct, "i"), [&](const match &) { return lc(fct); });
                if (o.uc_functions == 2) subst(token, re(fct, "i"), [&](const match &) { return uc(fct); });
                fct = ucfirst(lc(fct));
                if (o.uc_functions == 3) subst(token, re(fct, "i"), [&](const match &) { return fct; });
            }

            // case of (NEW|OLD).colname keyword that need to formatted too
            if ((is_in_create_function || truthy(fct_code_delimiter) || is_in_rule) && test(token, UR"~(^(NEW|OLD)\.)~", "i")) {
                if (o.uc_keywords == 1) subst(token, re(UR"~(^(OLD|NEW)\.)~", "i"), [](const match &x) { return lc(x[1]) + U"."; });
                if (o.uc_keywords == 2) subst(token, re(UR"~(^(OLD|NEW)\.)~", "i"), [](const match &x) { return uc(x[1]) + U"."; });
                if (o.uc_keywords == 3) subst(token, re(UR"~(^OLD\.)~", "i"), with(U"OLD."));
                if (o.uc_keywords == 3) subst(token, re(UR"~(^NEW\.)~", "i"), with(U"NEW."));
            }
        }

        const bool tk_is_type = is_type(token, last_token, next);
        if (test(token, U"^(AT|SET)$", "i")) not_a_type = 1;
        else if (!tk_is_type) not_a_type = 0;

        // Type are always lowercase
        if (!not_a_type && (is_in_create || is_in_declare || is_in_cast || is_in_type || is_in_alter)) {
            if ((tk_is_type && last_token) || (test(token, U"^(WITH|WITHOUT)$", "i") && test(next, U"^(time|timestamp)$", "i"))) {
                if (test(last, U"^(AS|RETURNS|INOUT|IN|OUT)$", "i") || !is_keyword(last) || is_type(last) || is_type(next)) {
                    if (o.uc_types == 1) token = lc(token);
                    if (o.uc_types == 2) token = uc(token);
                    if (o.uc_types == 3) token = ucfirst(lc(token));
                }
            }
        }

        // Add formatting for HTML output
        if (o.colorize && o.format == U"html") token = highlight_code(token, last_token, next);

        for (auto &c: cast) {
            const auto words = split(re(UR"~((\s+))~"), c);
            c.clear();
            for (const auto &word: words) {
                if (!is_type(token)) {
                    c += word;
                } else {
                    if (o.uc_types == 1) c += lc(word);
                    if (o.uc_types == 2) c += uc(word);
                    if (o.uc_types == 3) c += ucfirst(lc(word));
                }
            }
            token += U"::" + c;
        }

        // Format cast in function code
        if (test(token, U"::")) {
            str reg;
            for (std::size_t i = 0; i < types.size(); ++i) {
                if (i) reg += U'|';
                reg += types[i];
            }
            reg = UR"~((?:TIMESTAMP(\s*\(\s*\d+\s*\))? WITH TIME ZONE|TIMESTAMP(\s*\(\s*\d+\s*\))? WITHOUT TIME ZONE|CHARACTER VARYING|)~" + reg + U")";
            const auto &pattern = re(U"::(" + reg + U")", "igs");
            if (o.uc_types == 1) subst_all(token, pattern, [](const match &x) { return U"::" + lc(x[1]); });
            if (o.uc_types == 2) subst_all(token, pattern, [](const match &x) { return U"::" + uc(x[1]); });
            if (o.uc_types == 3) subst_all(token, pattern, [](const match &x) { return U"::" + ucfirst(lc(x[1])); });
        }

        // special case for MySQL
        if (test(token, UR"~(^(;|\$\$|\/\/)$)~") && test(content_, UR"~(DELIMITER\s*$)~")) {
            if (!test(content_, UR"~(DELIMITER\s$)~")) content_ += U" ";
        }
        content_ += token;

        // This can't be the beginning of a new line anymore.
        new_line = false;
    }

    str beautify::indent() {
        if (new_line) return repeat(o.space, o.spaces * level);
        // When this is not for identation force using space
        return U" ";
    }

    void beautify::new_line_() {
        if (!new_line) content_ += break_;
        new_line = true;
    }

    str beautify::next_token_skip_comment() {
        for (const auto &t: tokens)
            if (!test(t, UR"~(^\s*(--|\/\*))~")) return t;
        return U"0";
    }

    bool beautify::is_keyword(const opt &token_arg, opt next_token_arg, const opt &last_token) {
        if (!truthy(token_arg)) return false;
        str t = *token_arg;
        // Remove cast if any
        subst(t, re(U"::[^:]+$"), with(U""));
        const str next = next_token_arg.value_or(str{});
        const str last = last_token.value_or(str{});

        // Fix some false positive
        if (uc(t) == U"LEVEL" && uc(next) != U"SECURITY") return false;
        if (uc(t) == U"EVENT" && uc(next) != U"TRIGGER") return false;
        if (test(current_sql_stmt, U"(INSERT|UPDATE)") && test(next, UR"~(^[,\)=]$)~") && (!last_token || test(last, UR"~(^[,\(]$)~")) &&
            !test(t, U"^(TRUE|FALSE|DEFAULT|NULL|CURRENT_.*|LEXIZE|END)$", "i"))
            return false;
        if (test(t, U"^(LOGIN|RULE)$", "i") && !is_in_create && !is_in_alter && !is_in_drop && !is_in_rule) return false;
        if (uc(t) == U"COMMENT" && !test(next, U"^ON|IS$", "i")) return false;

        if (last_token) {
            if (uc(t) == U"KEY" && !test(last, U"^(PRIMARY|FOREIGN|PARTITION|NO)$", "i")) return false;
            if (test(t, U"^(BTREE|HASH|GIST|SPGIST|GIN|BRIN)$", "i") && !test(last, U"^(USING|BY)$", "i")) return false;
            if (uc(t) == U"NOTICE" && uc(last) != U"RAISE") return false;
            if ((is_in_type || is_in_create) && test(last, U"^(OF|FROM)$", "i")) return false;
            if (uc(last) == U"AS" &&
                !test(t, U"^(IDENTITY|SELECT|ENUM|TRANSACTION|UPDATE|DELETE|INSERT|MATERIALIZED|ON|VALUES|RESTRICTIVE|PERMISSIVE|UGLY|EXECUTE|STORAGE|OPERATOR|RANGE|NOT)$", "i"))
                return false;
            if (test(t, U"^(TYPE|SCHEMA)$", "i") && test(last, UR"~(^(COLUMN|\(|,|\||\)))~", "i")) return false;
            if (test(t, U"^TYPE$", "i") && !test(last, U"^(CREATE|DROP|ALTER|FOR)$", "i") && !is_in_alter && !type_set.contains(uc(next)))
                return false;
        }

        // False negative
        if (test(t, U"^QUERY$", "i") && last_token && uc(last) == U"RETURN") return true;
        return keyword_set.contains(uc(t));
    }

    bool beautify::is_type(const opt &token, const opt &, const opt &next_token) {
        if (!token) return false;
        if (next_token && test(*next_token, U"^(SEARCH)$", "i")) return false;
        if (test(*token, U"^(int4|int8|num|tstz|ts|date)range$", "i") && (!next_token || *next_token == U"(")) return false;

        static const char32_t *const composite_types[] = {U"VARYING", U"PRECISION", U"WITH", U"WITHOUT", U"ZONE"};
        // Typically case of a data type used as an object name
        if (next_token) {
            if (type_set.contains(uc(*token)) && type_set.contains(uc(*next_token)) && !in_list(composite_types, uc(*next_token)))
                return false;
        }
        str t = *token;
        subst(t, re(UR"~(\s*\(.*)~"), with(U"")); // remove any parameter to the type
        return type_set.contains(uc(t));
    }

    // Always false in pgFormatter 5.11: _is_sql_keyword greps $self->{'sql_keywords'}, which is never
    // set (the list is $self->{'dict'}->{'sql_keywords'}). Kept so for the same output.
    bool beautify::is_sql_keyword(const str &) { return false; }

    bool beautify::is_comment(const str &token) {
        return test(token, UR"~(^\s*((?:--)[\ \t\S]*|/\*[\ \t\r\n\S]*?\*/)$)~", "s");
    }

    opt beautify::is_function(const opt &token, const opt &last_token, const opt &next_token) {
        if (!truthy(token)) return std::nullopt;
        if (const auto m = find(*token, re(functions_re, "i"))) {
            // Check the context of the function
            if (last_token && next_token) {
                if (*next_token != U"(") return std::nullopt;
                if (is_in_create == 1) return std::nullopt;
            }
            return m[1];
        }
        return std::nullopt;
    }

    bool beautify::is_constant(const str &token) {
        return test(token, UR"~(^\d+$)~") || test(token, UR"~(^(['"`]).*\1$)~");
    }

    bool beautify::is_punctuation(const str &token) const {
        if (o.comma == U"start" && token == U",") return false;
        return test(token, UR"~(^[,;.\[\]]$)~");
    }

    str beautify::generate_anonymized_string(const str &before, const str &original, const str &after) {
        // Prevent dates from being anonymized
        if (test(original, UR"~(\A\d\d\d\d[/:-]\d\d[/:-]\d\d\z)~")) return original;
        if (test(original, UR"~(\A\d\d[/:-]\d\d[/:-]\d\d\d\d\z)~")) return original;
        // Prevent dates format like DD/MM/YYYY HH24:MI:SS from being anonymized
        if (test(original, UR"~(
        \A
        (?:FM|FX|TM)?
        (?:
            HH | HH12 | HH24
            | MI
            | SS
            | MS
            | US
            | SSSS
            | AM | A\.M\. | am | a\.m\.
            | PM | P\.M\. | pm | p\.m\.
            | Y,YYY | YYYY | YYY | YY | Y
            | IYYY | IYY | IY | I
            | BC | B\.C\. | bc | b\.c\.
            | AD | A\.D\. | ad | a\.d\.
            | MONTH | Month | month | MON | Mon | mon | MM
            | DAY | Day | day | DY | Dy | dy | DDD | DD | D
            | W | WW | IW
            | CC
            | J
            | Q
            | RM | rm
            | TZ | tz
            | [\s/:-]
        )+
        (?:TH|th|SP)?
        \z
    )~", "x"))
            return original;
        // Prevent interval from being anonymized
        if (truthy(before) && test(before, U"interval", "i")) return original;
        if (truthy(after) && test(after, UR"~(^\)*::interval)~", "i")) return original;

        static const sv chars = U"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz-_.";
        auto &cached = anonymization_cache[original];
        if (!truthy(cached)) {
            std::uniform_int_distribution<std::size_t> pick(0, chars.size() - 1);
            cached.clear();
            for (int i = 0; i < 10; ++i) cached += chars[pick(random)];
        }
        return cached;
    }

    void beautify::anonymize() {
        str q = query_;
        // just in case it has not been called in the main script
        if (!truthy(q)) q = query();
        if (!truthy(q)) return;
        anonymization_cache.clear();

        // Remove comments
        subst_all(q, re(UR"~(\/\*(.*?)\*\/)~", "gs"), with(U""));
        // Clean query
        subst_all(q, re(UR"~(\\')~", "gs"), with(U""));
        subst_all(q, re(UR"~(('')+)~", "gs"), with(U"$EMPTYSTRING$"));
        // Anonymize each values
        subst_all(q, re(UR"~(
        ([^\s\']+[\s\(]*)       # before
        '([^']*)'               # original
        ([\)]*::\w+)?           # after
    )~", "x"), [&](const match &x) { return x[1] + U"'" + generate_anonymized_string(x[1], x[2], x[3]) + U"'" + x[3]; });
        subst_all(q, re(UR"~(\$EMPTYSTRING\$)~", "gs"), with(U"''"));

        for (auto &[k, value]: keyword_constant) value = U"'" + generate_anonymized_string(U"", value, U"") + U"'";
        query(q);
    }

    void beautify::remove_dynamic_code(str &text, const str &code_sep) {
        std::vector<str> dynsep;
        if (truthy(code_sep) && code_sep != U"'") dynsep.push_back(code_sep);

        // Try to auto detect the string separator if none are provided: if a dollar sign is found
        // after EXECUTE then the following string until an other dollar is found will be
        // understand as a text delimiter
        if (dynsep.empty()) dynsep = find_all(text, re(UR"~(EXECUTE\s+(\$[^\$\s]*\$))~", "igs"));

        match m;
        for (const auto &sep: dynsep) {
            const auto quoted = quotemeta(sep);
            while (subst(text, re(U"(" + quoted + U".*?" + quoted + U")", "s"),
                         [&](const match &) { return U"TEXTVALUE" + number(idx_code) + U"E"; }, &m)) {
                dynamic_code[number(idx_code)] = m[1];
                ++idx_code;
            }
        }

        // Replace any COMMENT constant between single quote
        while (subst(text, re(UR"~(IS\s+('(?:.*?)')\s*;)~", "s"), [&](const match &) { return U"IS TEXTVALUE" + number(idx_code) + U"E;"; }, &m)) {
            dynamic_code[number(idx_code)] = m[1];
            ++idx_code;
        }

        // keep untouched parts between double single quotes
        while (subst(text, re(UR"~((PGFESCQ1(?:[^\r\n\|;]*?)PGFESCQ1))~", "s"),
                     [&](const match &) { return U"TEXTVALUE" + number(idx_code) + U"E"; }, &m)) {
            dynamic_code[number(idx_code)] = m[1];
            ++idx_code;
        }
    }

    void beautify::restore_dynamic_code(str &text) {
        subst_all(text, re(U"TEXTVALUE(\\d+)E", "gs"), [&](const match &x) {
            const auto found = dynamic_code.find(x[1]);
            return found != dynamic_code.end() ? found->second : str{};
        });
    }

    void beautify::quote_operator(str &text) {
        auto lines = split(re(UR"~([\n])~"), text);
        for (auto &line: lines) {
            match m;
            if (subst(line, re(UR"~(((?:CREATE|DROP|ALTER)\s+OPERATOR\s+(?:IF\s+EXISTS)?)\s*((:?[a-z0-9]+\.)?[\+\-\*\/<>=\~\!\@\#\%\^\&\|\`\?]+)\s*)~", "i"),
                      with(U"$1 \"$2\" "), &m)) {
                if (std::find(operators.begin(), operators.end(), m[2]) == operators.end()) operators.push_back(m[2]);
            }
        }
        str joined;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) joined += U'\n';
            joined += lines[i];
        }
        text = joined;

        long idx = 0;
        match m;
        while (subst(text, re(UR"~((NEGATOR|COMMUTATOR)\s*=\s*([^,\)\s]+))~", "is"),
                     [&](const match &x) { return uc(x[1]) + number(idx); }, &m)) {
            const str value = m[1] + U" = " + m[2];
            if (uc(m[1]) == U"COMMUTATOR") commutator[number(idx)] = value, has_commutator = true;
            else negator[number(idx)] = value, has_negator = true;
            ++idx;
        }
    }

    void beautify::restore_operator(str &text) {
        for (const auto &op: operators) subst_all(text, re(U"\"" + op + U"\"", "gs"), [&](const match &) { return op; });
        if (has_commutator)
            subst_all(text, re(U"COMMUTATOR(\\d+)", "igs"), [&](const match &x) {
                const auto found = commutator.find(x[1]);
                return found != commutator.end() ? found->second : str{};
            });
        if (has_negator)
            subst_all(text, re(U"NEGATOR(\\d+)", "igs"), [&](const match &x) {
                const auto found = negator.find(x[1]);
                return found != negator.end() ? found->second : str{};
            });
    }

    void beautify::quote_comment_stmt(str &text) {
        long idx = 0;
        match m;
        while (subst(text, re(UR"~((COMMENT\s+ON\s+(?:.*?)\s+IS)\s+(\$[^;]+?\$)\s*;)~", "is"),
                     [&](const match &x) { return x[1] + U" PGF_CMTSTR" + number(idx) + U";"; }, &m)) {
            comment_str[number(idx)] = m[2];
            has_comment_str = true;
            ++idx;
        }
    }

    void beautify::restore_comment_stmt(str &text) {
        if (!has_comment_str) return;
        subst_all(text, re(U"PGF_CMTSTR(\\d+)", "igs"), [&](const match &x) {
            const auto found = comment_str.find(x[1]);
            return found != comment_str.end() ? found->second : str{};
        });
    }

    void beautify::remove_comments() {
        long idx = 0;
        match m;
        while (subst(content_, re(UR"~((\/\*(.*?)\*\/))~", "s"), [&](const match &) { return U"PGF_COMMENT" + number(idx) + U"A"; }, &m)) {
            comments[U"PGF_COMMENT" + number(idx) + U"A"] = m[1];
            ++idx;
        }
        auto lines = split(re(U"\\n"), content_);
        std::vector<bool> deleted(lines.size(), false);
        for (long j = 0; j < static_cast<long>(lines.size()); ++j) {
            // Extract multiline comments as a single placeholder
            const long old_j = j;
            str cmt;
            while (j < static_cast<long>(lines.size())) {
                const auto c = find(lines[j], UR"~(^(\s*\-\-.*)$)~");
                if (!c) break;
                cmt += c[1] + U"\n";
                ++j;
            }
            if (j > old_j) {
                if (!cmt.empty() && cmt.back() == U'\n') cmt.pop_back(); // chomp
                const str key = U"PGF_COMMENT" + number(idx) + U"A";
                subst(lines[old_j], re(UR"~(^(\s*\-\-.*)$)~"), [&](const match &) { return key; });
                deleted[old_j] = false;
                comments[key] = cmt;
                ++idx;
                --j;
                while (j > old_j) {
                    lines[j].clear(); // delete $lines[$j] leaves an undefined element, joined as ""
                    deleted[j] = true;
                    --j;
                }
            }
            if (subst(lines[j], re(UR"~((\s*\-\-.*)$)~"), [&](const match &) { return U"PGF_COMMENT" + number(idx) + U"A"; }, &m)) {
                comments[U"PGF_COMMENT" + number(idx) + U"A"] = m[1];
                ++idx;
            }

            // Mysql supports differents kinds of comment's starter
            if (subst(lines[j], re(UR"~((\s*COMMENT\s+'.*)$)~"), [&](const match &) { return U"PGF_COMMENT" + number(idx) + U"A"; }, &m) ||
                subst(lines[j], re(UR"~((\s*\# .*)$)~"), [&](const match &) { return U"PGF_COMMENT" + number(idx) + U"A"; }, &m)) {
                str &value = comments[U"PGF_COMMENT" + number(idx) + U"A"];
                value = m[1];
                // Normalize start of comment
                subst(value, re(UR"~(^(\s*)COMMENT)~"), with(U"$1-- "));
                subst(value, re(UR"~(^(\s*)\#)~"), with(U"$1-- "));
                ++idx;
            }
        }
        // delete $array[$i] on the last elements shortens the array, as Perl does
        while (!lines.empty() && deleted.back()) {
            lines.pop_back();
            deleted.pop_back();
        }
        str joined;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) joined += U'\n';
            joined += lines[i];
        }
        content_ = joined;

        // Remove extra newline after comment
        while (subst(content_, re(UR"~((PGF_COMMENT\d+A[\n])[\n]+)~", "s"), with(U"$1"))) {}

        // Replace subsequent comment by a single one
        while (subst(content_, re(UR"~((PGF_COMMENT\d+A\s*PGF_COMMENT\d+A))~", "s"),
                     [&](const match &) { return U"PGF_COMMENT" + number(idx) + U"A"; }, &m)) {
            comments[U"PGF_COMMENT" + number(idx) + U"A"] = m[1];
            ++idx;
        }
    }

    void beautify::restore_comments(const bool wrap_comment) {
        if (o.wrap_limit && wrap_comment) {
            for (auto &[k, value]: comments) {
                if (test(value, UR"~(^(\s*)--[\r\n])~", "s")) continue;
                if (const auto c = find(value, UR"~(^(\s*)--)~")) {
                    const str indent_ = c[1];
                    if (static_cast<double>(value.size()) > o.wrap_limit + (o.wrap_limit * 10.0 / 100)) {
                        auto data = split(re(U"\\n"), value);
                        for (auto &d: data) subst(d, re(UR"~(^\s*--)~"), with(U""));
                        str joined;
                        for (std::size_t i = 0; i < data.size(); ++i) {
                            if (i) joined += U'\n';
                            joined += data[i];
                        }
                        const str t = text_wrap(U"", U" ", joined, o.wrap_limit);
                        data = split(re(U"\\n"), t);
                        joined.clear();
                        for (std::size_t i = 0; i < data.size(); ++i) {
                            if (i) joined += U'\n';
                            joined += indent_ + U"--" + data[i];
                        }
                        value = joined;
                    } else {
                        subst(value, re(UR"~(^\s*--)~", "s"), with(U""));
                        value = indent_ + U"--" + value;
                    }
                    // remove extra spaces after comment characters
                    subst_all(value, re(U"--[ ]+", "gs"), with(U"-- "));
                }
            }
        }

        match m;
        while (subst(content_, re(U"(PGF_COMMENT\\d+A)", "s"), [&](const match &x) {
            const auto found = comments.find(x[1]);
            return found != comments.end() ? found->second : str{};
        }, &m)) {
            comments.erase(m[1]);
        }
    }

    void beautify::wrap_lines(const bool wrap_comment) {
        if (!o.wrap_limit || !truthy(content_)) return;
        remove_comments();
        const auto lines = split(re(U"\\n"), content_);
        content_.clear();
        for (str l: lines) {
            // Remove and store the indentation of the line
            str indent_;
            match m;
            if (subst(l, re(UR"~(^(\s+))~"), with(U""), &m)) indent_ = m[1];
            if (static_cast<double>(l.size()) > o.wrap_limit + (o.wrap_limit * 10.0 / 100)) {
                content_ += text_wrap(indent_, repeat(U" ", o.spaces) + indent_, l, o.wrap_limit) + U"\n";
            } else {
                content_ += indent_ + l + U"\n";
            }
        }
        if (truthy(content_)) restore_comments(wrap_comment || o.wrap_comment);
    }
}
