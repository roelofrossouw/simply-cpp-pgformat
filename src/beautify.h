#pragma once
// The port of pgFormatter 5.11's pgFormatter::Beautify (Gilles Darold, PostgreSQL Licence; with code
// from SQL::Beautify by Jonas Kramer, Artistic License 2.0): a token-by-token state machine that
// indents SQL and PL/pgSQL. It follows the Perl closely, function by function and in the same
// order, so a change upstream can be carried over by comparing the two; the names are Perl's
// ($self->{'_is_in_where'} is is_in_where). Where the Perl would die on an undefined value (its
// warnings are fatal), this carries on with an empty one.

#include "perl.h"

#include <deque>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <unordered_set>
#include <vector>

namespace sc::pgformat::detail {
    using opt = std::optional<str>;

    struct settings {
        long spaces = 4;
        str space = U" ";
        long uc_keywords = 2, uc_functions = 0, uc_types = 1;
        bool no_comments = false, no_grouping = false;
        str placeholder;
        bool multiline = false;
        str separator;
        str comma = U"end";
        bool comma_break = false;
        str format = U"text";
        bool colorize = true;
        bool format_type = false;
        long wrap_limit = 0, wrap_after = 0;
        bool wrap_comment = false, numbering = false, no_extra_line = false, keep_newline = false;
        bool no_space_function = false, redundant_parenthesis = false, vertical_align = false;
    };

    class beautify {
    public:
        explicit beautify(const settings &options);

        void add_keywords(const std::vector<str> &words);
        void add_functions(const std::vector<str> &words);
        str query(const opt &new_value = std::nullopt);
        void anonymize();
        void run(); // beautify()
        void wrap_lines(bool wrap_comment);
        str content(const opt &new_value = std::nullopt);

    private:
        settings o;
        str break_ = U"\n";
        // Dictionaries: keywords and types in their order (they become regular expressions too),
        // with sets for the lookups.
        std::vector<str> keywords, types, functions;
        std::unordered_set<str> keyword_set, type_set, pg_function_set;
        std::vector<std::pair<str, str>> symbols;
        // exists $symbols{$x} looks at the keys; grep(/^$x$/, %symbols) at keys and values.
        std::unordered_set<str> symbol_keys, symbol_keys_and_values;
        str functions_re;

        // Placeholders taken out of the query and put back into the content.
        str query_;
        str content_;
        long idx_code = 0;
        std::vector<str> placeholder_values;
        std::map<str, str> dynamic_code, keyword_constant, alias_constant, comment_str, comments;
        std::vector<str> operators;
        std::map<str, str> commutator, negator;
        bool has_commutator = false, has_negator = false, has_comment_str = false;
        std::map<str, str> anonymization_cache;
        std::mt19937 random{std::random_device{}()};

        std::deque<str> tokens;
        long level = 0;
        std::vector<long> level_stack, level_parenthesis, level_parenthesis_function, begin_level, is_in_case;
        bool new_line = true;
        str current_sql_stmt, current_full_sql_stmt, fct_code_delimiter, current_function, mysql_delimiter;
        long is_meta_command = 0, language_sql = 0, first_when_in_case = 0, is_in_if = 0, is_in_return_query = 0;
        long is_in_set = 0, is_in_conversion = 0, is_in_where = 0, is_in_from = 0, is_in_join = 0, is_in_create = 0;
        long is_in_create_table = 0, is_in_create_schema = 0, is_in_rule = 0, is_in_create_function = 0;
        long is_in_drop_function = 0, is_in_alter = 0, is_in_trigger = 0, is_in_publication = 0, is_in_call = 0;
        long is_in_type = 0, is_in_domain = 0, is_in_declare = 0, is_in_block = -1, is_in_work = 0, is_in_function = 0;
        long is_in_merge = 0, is_in_statistics = 0, is_in_cast = 0, is_in_procedure = 0, is_in_index = 0;
        long is_in_with = 0, is_in_explain = 0, is_in_overlaps = 0, parenthesis_level = 0, parenthesis_function_level = 0;
        long has_order_by = 0, is_in_order_by = 0, has_over_in_join = 0, insert_values = 0, is_in_constraint = 0;
        long is_in_distinct = 0, is_in_array = 0, is_in_filter = 0, parenthesis_filter_level = 0, is_in_within = 0;
        long is_in_grouping = 0, is_in_partition = 0, is_in_over = 0, is_in_policy = 0, policy_as_using = 0;
        long is_in_truncate = 0, is_in_using = 0, and_level = 0, col_count = 0, is_in_drop = 0, is_in_operator = 0;
        long is_in_exception = 0, is_in_sub_query = 0, is_in_fetch = 0, is_in_aggregate = 0, is_in_value = 0;
        long parenthesis_level_value = 0, parenthesis_with_level = 0, is_in_returns_table = 0, has_limit = 0;
        long not_a_type = 0, stmt_number = 1, is_subquery = 0, is_in_generated = 0, is_in_between = 0;
        long is_in_materialized = 0, is_closing_function = 0, is_in_returning = 0, no_break = 0;

        void refresh_functions_re();
        str highlight_code(str token, const opt &last_token, const opt &next_token);
        std::vector<str> tokenize_sql(const opt &query = std::nullopt);

        // Vertical alignment of CREATE TABLE columns (a pass over the finished text).
        struct column {
            bool original = false;
            str line;
            str indent, name, comma, comment, declaration, remainder, rendered;
            std::vector<str> declaration_tokens, remainder_tokens;
        };
        std::optional<column> parse_create_table_column(const str &line);
        str render_sql_tokens(const std::vector<str> &tokens);
        static str pad_right(const str &value, long width);
        void align_create_table_column_keyword(std::vector<column> &rows, sv keyword);
        long parenthesis_delta(const str &line);
        void create_table_start(const str &line, bool &is_create_table, bool &has_opening, long &opening_depth);
        struct body_line {
            str line;
            long depth_before = 0, depth_after = 0;
        };
        std::vector<str> align_create_table_body(const std::vector<body_line> &body);
        str align_create_table_columns(const str &content);
        void align_create_table_column_comments(std::vector<column> &rows);
        std::vector<str> align_create_table_column_group(const std::vector<str> &lines);

        long pop_level();
        void reset_level();
        void set_level(const std::optional<long> &position);
        void push_level(long position);

        void add_token(str token, const opt &last_token = std::nullopt);
        void over() { ++level; }
        void back() {
            if (level > 0) --level;
        }
        str indent();
        void new_line_();
        [[nodiscard]] opt next_token() const { return tokens.empty() ? std::nullopt : opt{tokens.front()}; }
        [[nodiscard]] str nt() const { return tokens.empty() ? str{} : tokens.front(); }
        [[nodiscard]] bool has_next() const { return !tokens.empty(); }
        [[nodiscard]] str tk(std::size_t i) const { return i < tokens.size() ? tokens[i] : str{}; }
        [[nodiscard]] long last_index() const { return static_cast<long>(tokens.size()) - 1; }
        str next_token_skip_comment();

        bool is_keyword(const opt &token, opt next_token = std::nullopt, const opt &last_token = std::nullopt);
        bool is_type(const opt &token, const opt &last_token = std::nullopt, const opt &next_token = std::nullopt);
        bool is_sql_keyword(const str &token);
        static bool is_comment(const str &token);
        opt is_function(const opt &token, const opt &last_token = std::nullopt, const opt &next_token = std::nullopt);
        static bool is_constant(const str &token);
        bool is_punctuation(const str &token) const;
        str generate_anonymized_string(const str &before, const str &original, const str &after);

        void remove_dynamic_code(str &text, const str &code_sep);
        void restore_dynamic_code(str &text);
        void quote_operator(str &text);
        void restore_operator(str &text);
        void quote_comment_stmt(str &text);
        void restore_comment_stmt(str &text);
        void remove_comments();
        void restore_comments(bool wrap_comment);
    };
}
