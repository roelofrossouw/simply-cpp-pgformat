// pgFormatter::Beautify::beautify(): the token loop, in Beautify.pm's order. Perl's next is
// continue here, so the "$last = $token" at the bottom of the loop is skipped where Perl skips it.

#include "beautify.h"

#include <algorithm>

namespace sc::pgformat::detail {
    namespace {
        bool t(const sv s, const sv pattern) { return test(s, pattern); }
        bool ti(const sv s, const sv pattern) { return test(s, pattern, "i"); }

        const char32_t *const have_from_clause[] = {U"extract", U"overlay", U"substring", U"trim"};
        const char32_t *const extract_keywords[] = {
            U"century", U"day", U"decade", U"dow", U"doy", U"epoch", U"hour", U"isodow", U"isoyear", U"microseconds",
            U"millennium", U"minute", U"month", U"quarter", U"second", U"timezone", U"timezone_minute", U"week", U"year"};

        template<typename List>
        bool in_list(const List &list, const sv word) {
            return std::any_of(std::begin(list), std::end(list), [&](const char32_t *item) { return word == item; });
        }

        str join_tokens(const std::deque<str> &list) {
            str out;
            for (std::size_t i = 0; i < list.size(); ++i) {
                if (i) out += U' ';
                out += list[i];
            }
            return out;
        }
    }

    void beautify::run() {
        long pos = 0;

        // Main variables used to store differents state
        content(str{});
        level = 0;
        level_stack.clear();
        level_parenthesis.clear();
        new_line = true;
        current_sql_stmt.clear();
        current_full_sql_stmt.clear();
        is_meta_command = 0;
        fct_code_delimiter.clear();
        language_sql = 0;
        first_when_in_case = 0;
        is_in_if = 0;
        is_in_return_query = 0;
        is_in_set = 0;
        is_in_conversion = 0;
        is_in_case.clear();
        is_in_where = is_in_from = is_in_join = is_in_create = is_in_create_table = is_in_create_schema = 0;
        is_in_rule = is_in_create_function = is_in_drop_function = is_in_alter = is_in_trigger = is_in_publication = 0;
        is_in_call = is_in_type = is_in_domain = is_in_declare = 0;
        is_in_block = -1;
        is_in_work = is_in_function = is_in_merge = 0;
        current_function.clear();
        is_in_statistics = is_in_cast = is_in_procedure = is_in_index = is_in_with = is_in_explain = is_in_overlaps = 0;
        parenthesis_level = parenthesis_function_level = has_order_by = is_in_order_by = has_over_in_join = 0;
        insert_values = is_in_constraint = is_in_distinct = is_in_array = is_in_filter = parenthesis_filter_level = 0;
        is_in_within = is_in_grouping = is_in_partition = is_in_over = is_in_policy = policy_as_using = 0;
        is_in_truncate = is_in_using = and_level = col_count = is_in_drop = is_in_operator = is_in_exception = 0;
        is_in_sub_query = is_in_fetch = is_in_aggregate = is_in_value = parenthesis_level_value = 0;
        parenthesis_with_level = is_in_returns_table = has_limit = not_a_type = 0;
        stmt_number = 1;
        is_subquery = 0;
        mysql_delimiter.clear();
        is_in_generated = is_in_between = is_in_materialized = is_closing_function = is_in_returning = 0;
        begin_level.clear();

        str last;
        {
            auto list = tokenize_sql();
            tokens.assign(list.begin(), list.end());
        }

        if (o.numbering && last_index() > 0) content_ += U"-- Statement # " + number(stmt_number) + U"\n";
        while (!tokens.empty()) {
            str token = tokens.front();
            tokens.pop_front();
            is_closing_function = 0;

            if (o.keep_newline && is_in_block >= 0 && test(token, UR"~(^[\r\n]+$)~", "s") && last == U";") {
                add_token(token, last);
                continue;
            }

            // Replace concat operator found in some SGBD into || for normalization
            if (lc(token) == U"concat" && has_next() && nt() != U"(") token = U"||";

            // Case where a keyword is used as a column name.
            if (is_in_create > 1 && is_keyword(token, next_token(), last) && has_next() && is_type(next_token())) {
                add_token(token, last);
                last = token;
                continue;
            }

            // COPY block
            if (ti(token, UR"~(^COPY\s+[^\s]+\s+\()~")) {
                new_line_();
                add_token(token, last);
                new_line_();
                content_ += U"\n";
                last = token;
                continue;
            }

            if (uc(token) == U"BETWEEN") {
                is_in_between = 1;
                add_token(token, last);
                last = token;
                continue;
            }

            // mark when we are processing a materialized view to avoid formating issue with parameters
            if (uc(token) == U"MATERIALIZED" && uc(nt()) == U"VIEW") is_in_materialized = 1;

            // Find if the current keyword is a known function name
            if (truthy(last) && has_next() && nt() == U"(") {
                str word = lc(token);
                subst(word, re(UR"~(^[^\.]+\.)~"), with(U""));
                subst(word, re(U"^::"), with(U""));
                if (uc(last) == U"FUNCTION" && t(token, UR"~(^\d+$)~")) {
                    ++is_in_function;
                } else if (truthy(word) && pg_function_set.contains(word)) {
                    current_function = word;
                    if (is_in_create != 1 || ti(token, U"^CAST$")) ++is_in_function;
                    // Try to detect user defined functions
                } else if (last != U"*" && !is_keyword(token, next_token(), last) && (symbol_keys.contains(last) || t(last, UR"~(^\d+$)~"))) {
                    ++is_in_function;
                } else if (uc(token) == U"IN" && !ti(tk(1), U"^(SELECT|WITH|VALUES)$")) {
                    ++is_in_function;
                    // try to detect if this is a user function
                } else if (!is_in_function && !is_in_create && !is_comment(token) && token.size() > 2 &&
                           !ti(last, U"^(?:AS|RECURSIVE|WITH|OPERATOR|INTO|TYPE|VIEW)") && !is_keyword(token, next_token(), last)) {
                    ++is_in_function;
                }
            }

            // Set open parenthesis position to know if we are in subqueries or function parameters
            if (token == U")") {
                if (parenthesis_filter_level) --parenthesis_filter_level;
                if (parenthesis_with_level) --parenthesis_with_level;
                if (!parenthesis_filter_level) is_in_filter = 0;

                if (!is_in_function) {
                    if (parenthesis_level > 0) --parenthesis_level;
                } else {
                    if (parenthesis_function_level > 0) {
                        is_closing_function = 1;
                        --parenthesis_function_level;
                    }
                    if (!parenthesis_function_level) {
                        long position = 0;
                        if (!level_parenthesis_function.empty()) {
                            position = level_parenthesis_function.back();
                            level_parenthesis_function.pop_back();
                        }
                        set_level(position);
                        if (!is_in_create && !is_in_operator && !is_in_alter && uc(nt()) != U"LOOP") over();
                    }
                }
                if (!parenthesis_function_level) is_in_function = 0;
                if ((!has_next() || !ti(nt(), U"^(WITH|WITHOUT)$")) && (!parenthesis_level || !parenthesis_function_level)) is_in_cast = 0;

                if (!parenthesis_level && is_in_sub_query && has_next() && (!is_in_order_by || ti(nt(), U"^(FROM|GROUP)$"))) {
                    --is_in_sub_query;
                    back();
                }
                if (is_in_value) {
                    if (parenthesis_level_value) --parenthesis_level_value;
                }
                if (has_next() && ti(nt(), U"^(;|ON)$")) is_in_create_table = 0;
            } else if (token == U"(") {
                if (is_in_filter) ++parenthesis_filter_level;
                if (is_in_with) ++parenthesis_with_level;
                if (is_in_function) {
                    ++parenthesis_function_level;
                    if (parenthesis_function_level == 1) level_parenthesis_function.push_back(level);
                } else {
                    if (!parenthesis_level && is_in_from) level_parenthesis.push_back(level);
                    ++parenthesis_level;
                    if (is_in_value) ++parenthesis_level_value;
                }
                if (has_next() && ti(nt(), U"^(SELECT|WITH)$")) {
                    if (uc(last) != U"AS") ++is_in_sub_query;
                }
            }

            // Control case where we have to add a newline, go back and reset indentation after the
            // last ) in the WITH statement
            if (ti(token, U"^WITH$") && (last != U")" && !ti(nt(), U"^(TIME|FUNCTION)"))) {
                if (!is_in_partition && !is_in_publication && !is_in_policy) {
                    if (!is_in_using && !is_in_materialized && uc(nt()) != U"ORDINALITY" && uc(last) != U"START") is_in_with = 1;
                    if (uc(nt()) == U"ORDINALITY") no_break = 1;
                }
                is_in_materialized = 0;
            } else if (ti(token, U"^WITH$") && uc(nt()) == U"ORDINALITY") {
                no_break = 1;
            } else if (ti(token, U"^(AS|IS)$") && has_next() && t(nt(), UR"~(^(NOT|\()$)~")) {
                is_in_materialized = 0;
                if (is_in_with == 1) ++is_in_with;
            } else if (is_in_create && ti(token, U"^AS$") && has_next() && uc(nt()) == U"SELECT") {
                is_in_materialized = 0;
                is_in_create = 0;
            } else if (token == U"[") {
                ++is_in_array;
            } else if (token == U"]") {
                if (is_in_array) --is_in_array;
            } else if (token == U")") {
                has_order_by = 0;
                if (is_in_distinct) {
                    add_token(token);
                    new_line_();
                    is_in_distinct = 0;
                    last = token;
                    continue;
                }
                if (is_in_create && parenthesis_level == 1) is_in_generated = 0;
                if (is_in_using && !parenthesis_level && !is_in_policy) is_in_using = 0;
                if (has_next() && !ti(nt(), U"^(AS|WITH|,)$") && (!is_comment(nt()) || (last_index() >= 1 && tk(1) != U",")) &&
                    !parenthesis_with_level && is_in_with) {
                    is_in_with = 0;
                    is_in_from = 0;
                }

                if (is_in_create > 1 && has_next() && uc(nt()) == U"AS" && !is_in_with) {
                    is_in_materialized = 0;
                    if (last != U"(" && !is_in_create) new_line_();
                    if (is_in_returns_table && !parenthesis_level) {
                        is_in_returns_table = 0;
                        new_line_();
                        back();
                        add_token(token, last);
                        last = token;
                        continue;
                    } else {
                        if (is_in_procedure) over();
                    }
                }
                if ((is_in_with > 1 || is_in_operator) && !parenthesis_level && !parenthesis_with_level && !is_in_alter && !is_in_policy) {
                    if (!is_in_operator || (!is_in_drop && nt() == U";")) new_line_();
                    if (!is_in_operator) {
                        set_level(pop_level());
                        back();
                    }
                    add_token(token);
                    if (!is_in_operator) reset_level();
                    if (is_in_with) is_in_with = has_next() && nt() == U"," ? 1 : 0;
                    last = token;
                    continue;
                }
            } else if (has_next() && nt() == U"(") {
                if (uc(token) == U"FILTER") is_in_filter = 1;
                if (ti(token, U"^(GROUPING|ROLLUP)$")) is_in_grouping = 1;
            } else if (uc(token) == U"PASSING" && has_next() && uc(nt()) == U"BY") {
                has_order_by = 1;
            }

            // Explain need indentation in option list
            if (uc(token) == U"EXPLAIN") is_in_explain = 1;
            else if (uc(token) == U"OVERLAPS") is_in_overlaps = 1;

            if (current_sql_stmt == U"DO UPDATE" && ti(token, U"^(WHERE|SELECT)$")) {
                current_sql_stmt = U"INSERT";
                is_in_set = 0;
            }

            // Set the current kind of statement parsed
            if (const auto kind = find(token, U"^(FUNCTION|PROCEDURE|SEQUENCE|INSERT|DELETE|UPDATE|SELECT|RAISE|ALTER|GRANT|REVOKE|COMMENT|DROP|RULE|COMMENT|LOCK)$", "i");
                !is_in_policy && kind) {
                const str k_stmt = uc(kind[1]);
                is_in_explain = 0;
                is_in_where = 0;
                current_full_sql_stmt.clear();

                // Set current statement with taking care to exclude of SELECT ... FOR UPDATE
                // statement and ON CONFLICT DO UPDATE and FOR UPDATE TO.
                if (k_stmt != U"UPDATE" || (has_next() && nt() != U";" && nt() != U")" && !ti(last, U"^(DO|SHARE)$"))) {
                    if (!ti(k_stmt, U"^(UPDATE|DELETE)$") || !is_in_create) {
                        if (!ti(current_sql_stmt, U"^(GRANT|REVOKE)$") && !is_in_trigger && !is_in_merge && !is_in_operator && !is_in_alter) {
                            if (k_stmt != U"COMMENT" || ti(nt(), U"^(ON|IS)$")) {
                                if (uc(last) != U"WITH") current_sql_stmt = k_stmt;
                            }
                        }
                    }
                    if (ti(current_sql_stmt, U"^(INSERT|DELETE|UPDATE|SELECT|MERGE)$")) {
                        current_full_sql_stmt = current_sql_stmt + U" ";
                        for (const auto &next: tokens) {
                            if (next == U";") break;
                            current_full_sql_stmt += U" " + next;
                        }
                        current_full_sql_stmt += U";";
                    }
                }
            }

            // Mark that we are in CREATE statement that need newline after a comma in the parameter,
            // declare or column lists.
            if (ti(token, U"^(FUNCTION|PROCEDURE)$") && is_in_create && !is_in_trigger) {
                is_in_create_function = 1;
            } else if (ti(token, U"^(FUNCTION|PROCEDURE)$") && uc(last) == U"DROP") {
                is_in_drop_function = 1;
            } else if (ti(token, U"^(FUNCTION|PROCEDURE)$") && is_in_trigger) {
                is_in_index = 1;
            }
            if (ti(token, U"^CREATE$") && has_next() &&
                !ti(nt(), U"^(EVENT|UNIQUE|INDEX|EXTENSION|TYPE|PUBLICATION|OPERATOR|RULE|CONVERSION|DOMAIN)$")) {
                if (ti(nt(), U"^SCHEMA$")) {
                    is_in_create_schema = 1;
                } else if (is_in_create_schema) {
                    // we are certainly in a create schema statement
                    new_line_();
                    level = 1;
                    ++is_in_create_schema;
                }
                is_in_create = 1;
                if (has_next() && ti(nt(), U"(TABLE|TEMP)")) is_in_create_table = 1;
            } else if (ti(token, U"^CREATE$") && has_next() && ti(nt(), U"^RULE$")) {
                is_in_rule = 1;
            } else if (ti(token, U"^CREATE$") && has_next() && ti(nt(), U"^EVENT$")) {
                is_in_trigger = 1;
            } else if (ti(token, U"^CREATE$") && has_next() && ti(nt(), U"^TYPE$")) {
                is_in_type = 1;
            } else if (ti(token, U"^CREATE$") && has_next() && ti(nt(), U"^DOMAIN$")) {
                is_in_domain = 1;
            } else if (ti(token, U"^CREATE$") && has_next() && ti(nt(), U"^PUBLICATION$")) {
                is_in_publication = 1;
            } else if (ti(token, U"^CREATE$") && has_next() && ti(nt(), U"^CONVERSION$")) {
                is_in_conversion = 1;
            } else if (ti(token, U"^(CREATE|DROP)$") && has_next() && ti(nt(), U"^OPERATOR$")) {
                is_in_operator = 1;
                if (ti(token, U"^DROP$")) is_in_drop = 1;
            } else if (ti(token, U"^ALTER$")) {
                ++is_in_alter;
            } else if (ti(token, U"^DROP$")) {
                is_in_drop = 1;
            } else if (ti(token, U"^VIEW$") && is_in_create) {
                is_in_index = 1;
                is_in_create = 0;
            } else if (ti(token, U"^STATISTICS$") && is_in_create) {
                is_in_statistics = 1;
                is_in_create = 0;
            } else if (ti(token, U"^CAST$") && has_next() && nt() == U"(") {
                is_in_cast = 1;
            } else if (ti(token, U"^AGGREGATE$") && is_in_create) {
                is_in_aggregate = 1;
                has_order_by = 1;
            } else if (ti(token, U"^EVENT$") && has_next() && ti(nt(), U"^TRIGGER$")) {
                over();
                is_in_index = 1;
            } else if (ti(token, U"^CREATE$") && has_next() && ti(nt(), U"^INDEX|UNIQUE")) {
                if (is_in_create_schema) {
                    new_line_();
                    level = 1;
                    ++is_in_create_schema;
                }
            } else if (ti(token, U"^MERGE$") && has_next() && ti(nt(), U"^INTO")) {
                is_in_merge = 1;
                current_sql_stmt = U"MERGE";
            }

            if (is_in_using && has_next() && ti(nt(), U"^(OPERATOR|AS)$")) is_in_using = 0;

            if (ti(token, U"^ALTER$") && is_in_alter > 1) {
                new_line_();
                if (last != U",") over();
                add_token(token);
                last = token;
                continue;
            }

            // Special case for MySQL delimiter
            if (uc(token) == U"DELIMITER" && has_next() && (nt() == U"//" || nt() == U"$$")) {
                mysql_delimiter = nt();
            } else if (uc(token) == U"DELIMITER" && has_next() && nt() == U";") {
                mysql_delimiter.clear();
            }

            // case of the delimiter alone
            if (truthy(mysql_delimiter) && token == mysql_delimiter) {
                subst(content_, re(U"\\n\\n$", "s"), with(U"\n"));
                add_token(token);
                new_line_();
                last = U";";
                continue;
            }

            // Mark that we are in a CALL statement to remove any new line
            if (ti(token, U"^CALL$")) is_in_call = 1;

            // Increment operator tag to add newline in alter operator statement
            if ((is_in_alter || uc(last) == U"AS") && uc(token) == U"OPERATOR") {
                if (uc(last) == U"AS" && uc(token) == U"OPERATOR") new_line_();
                ++is_in_operator;
            }

            // Mark that we are in index/constraint creation statement to avoid inserting a newline
            // after comma and AND/OR keywords. This also used in SET statement taking care that we
            // are not in update statement. CREATE statement are not subject to this rule
            if (!is_in_create && ti(token, U"^(INDEX|PRIMARY|CONSTRAINT)$")) {
                if (ti(last, U"^(ALTER|CREATE|UNIQUE|USING|ADD)$")) is_in_index = 1;
            } else if (!is_in_create && uc(token) == U"SET") {
                if (current_sql_stmt != U"UPDATE") is_in_index = 1;
            } else if (is_in_create && (uc(token) == U"UNIQUE" || (ti(token, U"^(PRIMARY|FOREIGN)$") && uc(nt()) == U"KEY"))) {
                is_in_constraint = 1;
            }
            // Same as above but for ALTER FUNCTION/PROCEDURE/SEQUENCE or when we are in a CREATE
            // FUNCTION/PROCEDURE statement
            else if (ti(token, U"^(FUNCTION|PROCEDURE|SEQUENCE)$") && !is_in_trigger) {
                if (uc(last) == U"ALTER" && !is_in_operator && !is_in_alter) is_in_index = 1;
                if (ti(token, U"^FUNCTION$") && (is_in_create || current_sql_stmt == U"COMMENT")) {
                    if (!is_in_operator) is_in_index = 1;
                } else if (ti(token, U"^PROCEDURE$") && is_in_create) {
                    is_in_index = 1;
                    is_in_procedure = 1;
                }
            }
            // Desactivate index like formatting when RETURN(S) keyword is found
            else if (ti(token, U"^(RETURN|RETURNS)$")) {
                is_in_index = 0;
                if (uc(token) == U"RETURNS" && uc(nt()) == U"TABLE") is_in_returns_table = 1;
            } else if (ti(token, U"^AS$")) {
                is_in_materialized = 0;
                if (!is_in_index && is_in_from && last == U")" && uc(token) == U"AS" && nt() == U"(") is_in_index = 1;
                else is_in_index = 0;
                if (is_in_procedure) is_in_block = 1;
                is_in_over = 0;
            }

            if (ti(token, U"^(BEGIN|DECLARE)$")) {
                if (is_in_create) --is_in_create;
                if (uc(token) == U"BEGIN") begin_level.push_back(begin_level.empty() ? 0 : level);
            }

            // Mark statements that use string_agg() or group_concat() function as statement that can
            // have an ORDER BY clause inside the call to prevent applying order by formatting.
            if (ti(token, U"^(string_agg|group_concat|array_agg|percentile_cont)$")) {
                has_order_by = 1;
            } else if (ti(token, U"^(?:GENERATED)$") && ti(nt(), U"^(ALWAYS|BY)$")) {
                is_in_generated = 1;
            } else if (ti(token, U"^(?:TRUNCATE)$")) {
                no_break = 1;
            } else if (uc(token) == U"IDENTITY") {
                has_order_by = 0;
                no_break = 0;
                is_in_generated = 0;
            } else if (has_order_by && uc(token) == U"ORDER" && ti(nt(), U"^BY$")) {
                add_token(token, last);
                last = token;
                continue;
            } else if (has_order_by && uc(token) == U"BY") {
                add_token(token);
                last = token;
                continue;
            } else if (ti(token, U"^OVER$")) {
                add_token(token);
                is_in_over = 1;
                has_order_by = 1;
                last = token;
                continue;
            }

            // Fix case where we don't knwon if we are outside a SQL function
            if (uc(last) == U"AS" && has_next() && nt() == U";" && is_in_create_function) is_in_create_function = 0;

            // Set function code delimiter, it can be any string found after the AS keyword in
            // function or procedure creation code. Toogle _fct_code_delimiter to force next token to
            // be stored as the function code delimiter
            if (uc(token) == U"AS" && (!truthy(fct_code_delimiter) || t(nt(), U"CODEPART")) && ti(current_sql_stmt, U"^(FUNCTION|PROCEDURE)$")) {
                if (is_in_create && !is_in_with && !is_in_cast && !ti(nt(), U"^(IMPLICIT|ASSIGNMENT)$")) {
                    new_line_();
                    add_token(token);
                    is_in_set = 0;
                    if (!t(nt(), U"CODEPARTB")) reset_level();
                } else {
                    add_token(token);
                }
                if (!t(nt(), U"(CODEPART|IMPLICIT|ASSIGNMENT)") || t(nt(), U"^'")) {
                    if (!is_in_cast && is_in_create) {
                        // extract potential code joined with the code separator
                        if (!tokens.empty() && subst(tokens[0], re(U"^'(.)"), with(U"$1"))) {
                            subst(tokens[0], re(UR"~([;\s]*'$)~"), with(U""));
                            auto tmp_arr = tokenize_sql(tokens[0]);
                            tmp_arr.push_back(U";");
                            tmp_arr.push_back(U"'");
                            tokens.pop_front();
                            tokens.insert(tokens.begin(), tmp_arr.begin(), tmp_arr.end());
                            tokens.push_front(U"'");
                        }
                        fct_code_delimiter = U"1";
                    }
                }
                is_in_create = 0;
                last = token;
                continue;
            } else if (ti(token, U"^(INSTEAD|ALSO)$") && uc(last) == U"DO") {
                add_token(token);
                new_line_();
                last = token;
                continue;
            } else if (ti(token, U"^DO$") && has_next() && ti(nt(), U"^(INSTEAD|ALSO|UPDATE|NOTHING)$")) {
                new_line_();
                over();
                add_token(token);
                last = token;
                continue;
            } else if (ti(token, U"^DO$") && !truthy(fct_code_delimiter) && t(nt(), UR"~(^\$[^\s]*)~")) {
                begin_level.clear();
                fct_code_delimiter = U"1";
                is_in_create_function = 1;
                if (!test(content_, U"\\n$", "s")) new_line_();
                add_token(token);
                last = token;
                is_in_procedure = 1;
                continue;
            }

            // Store function code delimiter
            if (fct_code_delimiter == U"1") {
                if (t(nt(), U"CODEPART")) fct_code_delimiter = U"0";
                else if (t(token, U"^'.*'$")) fct_code_delimiter = U"'";
                else fct_code_delimiter = token;
                add_token(token);
                if (!is_in_create_function || nt() != U"," || !t(tk(1), U"KEYWCONST")) new_line_();
                if (has_next() && !ti(next_token_skip_comment(), U"^(DECLARE|BEGIN)$")) {
                    over();
                    language_sql = 1;
                }

                if (fct_code_delimiter == U"'") {
                    is_in_block = -1;
                    is_in_exception = 0;
                    if (nt() == U";") reset_level();
                    fct_code_delimiter.clear();
                    current_sql_stmt.clear();
                    is_in_procedure = 0;
                    is_in_function = 0;
                    is_in_create_function = 0;
                    language_sql = 0;
                }
                last = token;
                continue;
            }

            // With SQL language the code delimiter can be include with the keyword, try to detect
            // and fix it
            if (truthy(fct_code_delimiter) && subst(token, re(U"(.)" + quotemeta(fct_code_delimiter) + U"$"), with(U"$1")))
                tokens.push_front(fct_code_delimiter);

            // Desactivate the block mode when code delimiter is found for the second time
            if (truthy(fct_code_delimiter) && token == fct_code_delimiter) {
                is_in_block = -1;
                is_in_exception = 0;
                is_in_create_function = 0;
                reset_level();
                fct_code_delimiter.clear();
                current_sql_stmt.clear();
                language_sql = 0;
                new_line_();
                add_token(token);
                last = token;
                continue;
            }

            // Mark when we are parsing a DECLARE or a BLOCK section. When entering a BLOCK section
            // store the current indentation level
            if (uc(token) == U"DECLARE" && is_in_create_function) {
                is_in_block = -1;
                is_in_exception = 0;
                is_in_declare = 1;
                reset_level();
                new_line_();
                add_token(token);
                new_line_();
                over();
                last = token;
                is_in_create_function = 0;
                continue;
            } else if (uc(token) == U"ATOMIC") {
                add_token(token);
                last = token;
                new_line_();
                over();
                ++is_in_block;
                // Store current indent position to print END at the right level
                push_level(level);
                continue;
            } else if (uc(token) == U"BEGIN") {
                is_in_declare = 0;
                if (is_in_block == -1) reset_level();
                new_line_();
                add_token(token);
                if (has_next() && !ti(nt(), U"^(WORK|TRANSACTION|ISOLATION|ATOMIC|;)$")) {
                    new_line_();
                    over();
                    ++is_in_block;
                    // Store current indent position to print END at the right level
                    push_level(level);
                }
                if (!o.no_grouping && has_next() && ti(nt(), U"^(WORK|TRANSACTION|ISOLATION|ATOMIC|;)$")) ++is_in_work;
                last = token;
                continue;
            } else if (ti(token, U"^(COMMIT|ROLLBACK)$") && uc(last) != U"ON" && !is_in_procedure) {
                is_in_work = 0;
                is_in_declare = 0;
                is_in_create_function = 0;
                new_line_();
                set_level(pop_level());
                add_token(token);
                last = token;
                begin_level.clear();
                continue;
            } else if (ti(token, U"^(COMMIT|ROLLBACK)$") && has_next() && nt() == U";" && is_in_procedure) {
                new_line_();
                add_token(token);
                last = token;
                begin_level.clear();
                continue;
            } else if (ti(token, U"^FETCH$") && last == U";") {
                new_line_();
                if (is_in_block == -1) back();
                add_token(token);
                last = token;
                is_in_fetch = 1;
                continue;
            }

            // Special case where we want to add a newline into ) AS (
            if (uc(token) == U"AS" && last == U")" && nt() == U"(") {
                new_line_();
            }
            // and before RETURNS with increasing indent level
            else if (uc(token) == U"RETURNS") {
                new_line_();
                if (uc(nt()) != U"NULL") over();
            }
            // and before WINDOW
            else if (uc(token) == U"WINDOW") {
                new_line_();
                set_level(pop_level());
                add_token(token);
                last = token;
                has_order_by = 1;
                continue;
            }

            // Treated DISTINCT as a modifier of the whole select clause, not only the first column only
            if (uc(token) == U"ON" && uc(last) == U"DISTINCT") {
                is_in_distinct = 1;
                over();
            } else if (uc(token) == U"DISTINCT" && uc(last) == U"SELECT" && has_next() && !ti(nt(), U"^ON$")) {
                add_token(token);
                if (!o.wrap_after) new_line_();
                over();
                last = token;
                continue;
            }

            // (No rules are defined: SQL::Beautify's custom rules aren't used by pg_format.)
            if (ti(token, U"^(LANGUAGE|SECURITY|COST)$") && !is_in_alter && !is_in_drop && last != U",") {
                begin_level.clear();
                if (uc(token) != U"SECURITY" || uc(last) != U"LEVEL") new_line_();
                add_token(token);
            } else if (ti(token, U"^PARTITION$") && !is_in_over && last != U"(") {
                is_in_partition = 1;
                if (is_in_create && last == U")") {
                    new_line_();
                    if (level) set_level(pop_level());
                    add_token(token);
                } else {
                    add_token(token);
                }
            } else if (ti(token, U"^POLICY$")) {
                is_in_policy = 1;
                add_token(token);
                last = token;
                continue;
            } else if ((ti(token, U"^TRUNCATE$") && !ti(nt(), U"^(TABLE|ONLY|,)$")) || (ti(token, U"^(TABLE|ONLY)$") && uc(last) == U"TRUNCATE")) {
                is_in_truncate = 1;
                add_token(token);
                last = token;
                str str_tmp = join_tokens(tokens);
                subst(str_tmp, re(U";.*", "s"), with(U""));
                if (test(str_tmp, U" , ", "s")) {
                    new_line_();
                    over();
                    is_in_truncate = 2;
                }
                continue;
            } else if (ti(token, U"^(RESTART|CASCADE)$") && is_in_truncate == 2) {
                new_line_();
                back();
                add_token(token);
                last = token;
                continue;
            } else if (ti(token, U"^TRIGGER$") && ti(last, U"^(CREATE|CONSTRAINT|REPLACE)$")) {
                is_in_trigger = 1;
                add_token(token);
                last = token;
                continue;
            } else if (ti(token, U"^(BEFORE|AFTER|INSTEAD)$") && is_in_trigger) {
                new_line_();
                over();
                add_token(token);
                last = token;
                continue;
            } else if (ti(token, U"^EXECUTE$") && (is_in_trigger || uc(last) == U"AS")) {
                new_line_();
                add_token(token);
                last = token;
                continue;
            } else if (token == U"(") {
                if (is_in_aggregate && has_next() && (is_keyword(next_token()) || is_sql_keyword(nt())) && uc(nt()) != U"VARIADIC") {
                    is_in_aggregate = 0;
                    has_order_by = 0;
                }
                if (is_in_create) ++is_in_create;
                if (is_in_constraint) ++is_in_constraint;
                // case of insert with multiple values, don't stay single line
                if (uc(last) == U"VALUES" && current_sql_stmt == U"INSERT" && t(current_full_sql_stmt, UR"~(\) , \()~")) {
                    new_line_();
                    over();
                }
                add_token(token, last);
                if (has_next() && uc(nt()) == U"SELECT") {
                    is_in_cast = 0;
                    ++is_subquery;
                }
                if (has_next() && nt() == U")" && !is_in_create) {
                    last = token;
                    continue;
                }
                if (!is_in_index && !is_in_publication && !is_in_distinct && !is_in_filter && !is_in_grouping && !is_in_partition &&
                    !is_in_over && !is_in_trigger && !is_in_policy && !is_in_aggregate && !no_break && !is_in_generated) {
                    if (uc(last) == U"AS" || is_in_create == 2 || uc(nt()) == U"CASE") {
                        if ((!is_in_function || ti(nt(), U"^CASE$")) && nt() != U")" && !ti(nt(), U"^(PARTITION|ORDER)$")) new_line_();
                    }
                    if (is_in_with == 1 || is_in_explain) {
                        over();
                        if (!o.wrap_after) new_line_();
                        if (!is_in_explain || o.wrap_after) last = token;
                        continue;
                    }
                    if (!is_in_if && !is_in_alter && (!is_in_function || last != U"(") && uc(last) != U"CHECK") {
                        if (is_in_operator <= 2 && is_in_create <= 2) over();
                        if (!is_in_function && !is_type(next_token())) {
                            if (is_in_operator == 1) {
                                new_line_();
                                ++is_in_operator;
                            } else if (is_in_type) {
                                new_line_();
                            }
                        }
                        last = token;
                    }
                    if (is_in_type == 1) {
                        last = token;
                        continue;
                    }
                }

                if (o.format_type && ti(current_sql_stmt, U"(FUNCTION|PROCEDURE)") && is_in_create == 2 && (!has_next() || nt() != U")")) {
                    if (is_in_block < 0) over();
                    new_line_();
                    continue;
                }
            } else if (token == U")") {
                if (has_next()) {
                    const str next = truthy(quotemeta(nt())) ? nt() : str{U"SELECT"};
                    if ((!parenthesis_level && has_next() && is_keyword(next_token())) || !symbol_keys_and_values.contains(next)) is_in_where = 0;
                }
                if (is_in_constraint && has_next() && (nt() == U"," || nt() == U")")) is_in_constraint = 0;
                else if (is_in_constraint) --is_in_constraint;

                // Case of CTE and explain
                if (is_in_with == 1 || is_in_explain) {
                    back();
                    if (!o.wrap_after && !is_in_overlaps) new_line_();
                    add_token(token);
                    if (token != U")" || (has_next() && uc(nt()) != U"AS")) last = token;
                    is_in_explain = 0;
                    continue;
                }
                if ((o.format_type && ti(current_sql_stmt, U"(FUNCTION|PROCEDURE)") && is_in_create == 2) || (has_next() && uc(nt()) == U"INHERITS")) {
                    if (is_in_block < 0) back();
                    if (last != U"(") new_line_();
                }
                if (is_in_index || is_in_alter || is_in_partition || is_in_policy || (has_next() && ti(nt(), U"^OVER$"))) {
                    add_token(U"");
                    add_token(token);
                    if (!parenthesis_level) is_in_over = 0;
                    last = token;
                    if (is_in_create) --is_in_create;
                    continue;
                }
                if (has_next() && !ti(nt(), U"FILTER")) {
                    bool add_nl = false;
                    if (is_in_create > 1 && last != U"(" && !is_in_cast &&
                        (!has_next() || ti(nt(), U"^(TABLESPACE|PARTITION|AS|;)$") || (ti(nt(), U"^ON$") && !parenthesis_level)))
                        add_nl = true;
                    if (is_in_type == 1 && !ti(nt(), U"^AS$") && (!has_next() || nt() == U";")) add_nl = true;
                    if (current_sql_stmt != U"INSERT" && !is_in_function && (has_next() && ti(nt(), U"^(SELECT|WITH)$")) &&
                        !ti(tk(1), U"^(ORDINALITY|FUNCTION)$") && (is_in_create || (last != U")" && last != U"]")) &&
                        (uc(nt()) != U"WITH" || !ti(uc(tk(1)), U"TIME|INOUT")))
                        add_nl = true;

                    if (add_nl) new_line_();

                    if (!is_in_grouping && !is_in_trigger && !no_break && !is_in_generated && is_in_create <= 2 && !ti(nt(), U"^LOOP$")) back();
                    if (is_in_create) --is_in_create;
                }
                if (!parenthesis_level) {
                    is_in_filter = 0;
                    is_in_within = 0;
                    is_in_grouping = 0;
                    is_in_over = 0;
                    has_order_by = 0;
                    is_in_policy = 0;
                    is_in_aggregate = 0;
                    policy_as_using = 0;
                }
                add_token(token);

                // Do not go further if this is the last token
                if (!has_next()) {
                    last = token;
                    continue;
                }

                // When closing CTE statement go back again
                if ((ti(nt(), U"^(?:SELECT|INSERT|UPDATE|DELETE)$") && !is_in_policy) || (is_in_with && is_subquery && is_subquery % 2 == 0)) {
                    if (current_sql_stmt != U"INSERT" && !is_closing_function &&
                        (!parenthesis_level || !has_next() || uc(nt()) == U"AS" || (last_index() >= 1 && tk(1) == U",")))
                        back();
                }
                if (is_subquery && has_next() && last_index() >= 1 && (uc(nt()) == U"AS" || tk(1) == U",")) --is_subquery;
                if (is_in_create <= 1) {
                    const str next_tok = quotemeta(nt());
                    if (has_next() && !ti(nt(), U"^(?:AS|IS|THEN|INTO|BETWEEN|ON|IN|FILTER|WITHIN|DESC|ASC|WITHOUT|CASCADE)$") &&
                        (!ti(nt(), U"^(AND|OR)$") || !is_in_if) && nt() != U")" && !t(nt(), U"^:") && nt() != U";" && nt() != U"," &&
                        nt() != U"||" && uc(nt()) != U"CONCAT" && (is_keyword(next_token()) || is_function(next_token())) &&
                        !t(current_sql_stmt, U"^(GRANT|REVOKE)$") && !symbol_keys.contains(next_tok) && !is_in_over && !is_in_cast &&
                        !is_in_domain && !is_in_merge)
                        new_line_();
                }
            } else if (token == U",") {
                bool add_newline = false;

                // Format INSERT with multiple values
                if (current_sql_stmt == U"INSERT" && last == U")" && nt() == U"(") {
                    if (o.comma == U"start") new_line_();
                    add_token(token);
                    if (o.comma == U"end") new_line_();
                    continue;
                }
                if (is_in_constraint == 1) is_in_constraint = 0;
                if (!is_in_function) ++col_count;
                if ((is_in_over || has_order_by) && !parenthesis_level && !parenthesis_function_level) {
                    is_in_over = 0;
                    has_order_by = 0;
                    back();
                }
                if ((!no_break && !is_in_generated && !is_in_function && !is_in_distinct && !is_in_array &&
                     (o.comma_break || current_sql_stmt != U"INSERT") && current_sql_stmt != U"RAISE" &&
                     (!t(current_sql_stmt, U"^(FUNCTION|PROCEDURE)$") || !fct_code_delimiter.empty()) && !is_in_where && !is_in_drop &&
                     !is_in_index && !is_in_aggregate && !is_in_alter && !is_in_publication && !is_in_call && !is_in_policy &&
                     !is_in_grouping && !is_in_partition && is_in_constraint <= 1 && is_in_create <= 2 && is_in_operator != 1 &&
                     !has_order_by && !t(current_sql_stmt, U"^(GRANT|REVOKE)$") &&
                     (!test(nt(), UR"~(^('$|\s*\-\-))~", "is") || (!test(nt(), U"^'$", "is") && o.no_comments)) &&
                     !parenthesis_function_level && (!col_count || col_count > (o.wrap_after - 1))) ||
                    (is_in_with && !o.wrap_after))
                    add_newline = true;
                if (col_count > (o.wrap_after - 1)) col_count = 0;
                if (is_in_using && parenthesis_level) add_newline = false;
                if (no_break) add_newline = false;

                if (is_in_with >= 1 && !parenthesis_level) {
                    if (!o.wrap_after) add_newline = true;
                }
                if (o.format_type && ti(current_sql_stmt, U"(FUNCTION|PROCEDURE)") && is_in_create == 2) add_newline = true;
                if (is_in_alter && is_in_operator >= 2) {
                    if (has_next() && ti(nt(), U"^(OPERATOR|FUNCTION)$")) add_newline = true;
                }
                if (is_in_returns_table) add_newline = true;
                if (add_newline && o.comma == U"start") new_line_();
                add_token(token);
                if (is_in_value && parenthesis_level_value) add_newline = false;
                if (is_in_function || is_in_statistics) add_newline = false;
                if (has_next() && !o.no_comments && is_comment(nt())) add_newline = false;
                if (has_next() && t(nt(), U"KEYWCONST") && last_index() >= 1 && ti(tk(1), U"^(LANGUAGE|STRICT)$")) add_newline = false;
                if (is_in_truncate) add_newline = true;
                if (add_newline && o.comma == U"end" && !is_in_merge &&
                    (o.comma_break || (current_sql_stmt != U"INSERT" && (current_sql_stmt != U"DO UPDATE" || !parenthesis_level))))
                    new_line_();
            } else if (token == U";" || t(token, UR"~(^\\(?:g|crosstabview|watch))~")) {
                // statement separator or executing psql meta command (prefix 'g' includes all its variants)
                add_token(token);

                if (token == U";" && !is_in_case.empty() && uc(last) != U"CASE") {
                    last = token;
                    continue;
                }
                if (is_in_type && last == U")") {
                    if (is_in_block == -1 && !parenthesis_level) reset_level();
                    --is_in_type;
                }
                if (is_in_rule) back();
                else if (is_in_create && is_in_block > -1) pop_level();
                else if (is_in_return_query) pop_level();

                // Initialize most of statement related variables
                no_break = 0;
                is_in_generated = is_in_where = is_in_between = is_in_from = is_in_join = is_in_create = 0;
                is_in_create_table = is_in_create_schema = is_in_alter = is_in_merge = is_in_rule = is_in_publication = 0;
                is_in_call = is_in_type = is_in_domain = is_in_function = 0;
                current_function.clear();
                is_in_index = is_in_statistics = is_in_cast = is_in_if = is_in_set = is_in_with = is_in_overlaps = 0;
                has_order_by = has_over_in_join = parenthesis_level = parenthesis_function_level = is_in_constraint = 0;
                is_in_distinct = is_in_array = is_in_filter = parenthesis_filter_level = is_in_partition = is_in_over = 0;
                is_in_policy = policy_as_using = is_in_truncate = is_in_trigger = is_in_using = and_level = col_count = 0;
                is_in_drop = is_in_conversion = is_in_operator = is_in_explain = is_in_sub_query = is_in_fetch = 0;
                is_in_aggregate = is_in_value = parenthesis_level_value = parenthesis_with_level = is_in_returns_table = 0;
                has_limit = not_a_type = is_subquery = is_in_order_by = is_in_materialized = is_in_drop_function = 0;
                current_full_sql_stmt.clear();
                is_in_return_query = 0;
                is_in_returning = 0;

                if (insert_values) {
                    if (is_in_block == -1 && !is_in_declare && !truthy(fct_code_delimiter)) {
                        reset_level();
                    } else if (is_in_block == -1 && current_sql_stmt == U"INSERT" && !is_in_create && !is_in_create_function) {
                        back();
                        pop_level();
                    } else {
                        set_level(pop_level());
                    }
                    insert_values = 0;
                }
                current_sql_stmt.clear();
                if (o.spaces == 0) break_ = U"\n";

                if (!ti(last, U"^VALUES$")) new_line_();

                // Add an additional newline after ; when we are not in a function
                if (is_in_block == -1 && !is_in_work && !language_sql && !is_in_declare && uc(last) != U"VALUES") {
                    new_line = false;
                    new_line_();
                    ++stmt_number;
                    if (o.numbering && last_index() > 0) content_ += U"-- Statement # " + number(stmt_number) + U"\n";
                }

                // End of statement; remove all indentation when we are not in a BEGIN/END block
                if (!is_in_declare && is_in_block == -1 && !truthy(fct_code_delimiter) && !is_in_exception) {
                    reset_level();
                } else if (!language_sql && (!has_next() || !t(nt(), U"^INSERT$")) && !is_in_exception) {
                    if (level_stack.empty()) set_level(is_in_declare ? 1 : is_in_block + 1);
                    else set_level(level_stack.back());
                }
                last = token;
            } else if (ti(token, U"^FOR$")) {
                if (is_in_policy) {
                    over();
                    new_line_();
                    add_token(token);
                    last = token;
                    continue;
                }
                if (ti(nt(), U"^(UPDATE|KEY|NO|VALUES)$")) {
                    if (!has_limit && (level_stack.empty() || level > level_stack.back())) back();
                    new_line_();
                    has_limit = 0;
                } else if (t(nt(), U"^EACH$") && is_in_trigger) {
                    new_line_();
                }
                add_token(token);
                // cover FOR in cursor
                if (uc(nt()) == U"SELECT") over();
                last = token;
            } else if (ti(token, U"^(?:FROM|WHERE|SET|RETURNING|HAVING|VALUES)$")) {
                if (uc(token) == U"FROM" && has_order_by && !parenthesis_level) {
                    if (has_order_by) back();
                }
                no_break = 0;
                col_count = 0;
                if (uc(token) == U"SET" && !ti(current_sql_stmt, U"^(UPDATE|INSERT)$")) is_in_set = 1;
                if (uc(token) == U"RETURNING") is_in_returning = 1;

                // special cases for create partition statement
                if (ti(token, U"^VALUES$") && ti(last, U"^(FOR|IN)$")) {
                    add_token(token);
                    no_break = 1;
                    last = token;
                    continue;
                } else if (ti(token, U"^FROM$") && uc(last) == U"VALUES") {
                    add_token(token);
                    last = token;
                    continue;
                }

                // Case of DISTINCT FROM clause
                if (ti(token, U"^FROM$")) {
                    if (uc(last) == U"DISTINCT" || is_in_fetch || is_in_alter || is_in_conversion || is_in_set) {
                        add_token(token);
                        last = token;
                        is_in_set = 0;
                        continue;
                    }
                    if (!is_in_function && !is_in_partition) ++is_in_from;
                }

                if (ti(token, U"^WHERE$") && !is_in_filter) {
                    if (has_over_in_join) back();
                    ++is_in_where;
                    if (is_in_from) --is_in_from;
                    is_in_join = 0;
                    has_over_in_join = 0;
                } else if (!is_in_function) {
                    if (is_in_where) --is_in_where;
                }

                if (ti(token, U"^SET$") && is_in_create) {
                    // Add newline before SET statement in function header
                    if (!ti(last, U"^(DELETE|UPDATE)$")) new_line_();
                } else if (ti(token, U"^WHERE$") && current_sql_stmt == U"DELETE") {
                    new_line_();
                    add_token(token);
                    over();
                    last = token;
                    is_in_join = 0;
                    continue;
                } else if (ti(token, U"^SET$") && uc(last) == U"UPDATE" && !is_keyword(next_token())) {
                    is_in_index = 0;
                    is_in_from = 0;
                    add_token(token);
                    if (!is_in_merge) new_line_();
                    over();
                    last = token;
                    continue;
                } else if (!ti(token, U"^FROM$") || (!is_in_function && !is_in_statistics && !t(current_sql_stmt, U"(DELETE|REVOKE)"))) {
                    if (!is_in_filter && (!ti(token, U"^SET$") || !is_in_index)) {
                        if ((uc(token) != U"VALUES" || (current_sql_stmt != U"INSERT" && last != U"'")) &&
                            (!ti(uc(token), U"^WHERE$") || is_in_with < 2 || level > 1))
                            back();
                        if (uc(token) == U"WHERE" && is_in_function && is_subquery <= 2) over();
                        if (!is_in_rule && !is_in_merge && (!ti(last, U"^DEFAULT$") || nt() != U";")) new_line_();
                    }
                } else {
                    if (uc(token) == U"FROM" && is_in_sub_query && !in_list(extract_keywords, lc(last)) && (insert_values || is_in_function) &&
                        (!is_in_function || !in_list(have_from_clause, current_function))) {
                        new_line_();
                        back();
                    }
                    if (uc(token) == U"FROM") current_function.clear();
                    add_token(token);
                    last = token;
                    continue;
                }

                if (ti(token, U"^VALUES$") && !is_in_rule && !o.comma_break && !is_in_merge && (current_sql_stmt == U"INSERT" || last == U"(")) {
                    if (uc(token) != U"VALUES" || current_sql_stmt != U"INSERT" || !t(current_full_sql_stmt, UR"~(\) , \()~")) over();
                    if (current_sql_stmt == U"INSERT" || last == U"(") {
                        insert_values = 1;
                        push_level(level);
                    }
                }

                if (!is_in_merge && ti(token, U"^VALUES$") && last == U"(") is_in_value = 1;

                if (uc(token) == U"WHERE") {
                    add_token(token, last);
                    is_in_value = 0;
                    parenthesis_level_value = 0;
                } else {
                    add_token(token);
                }

                if (ti(token, U"^VALUES$") && last == U"(") {
                    over();
                } else if (ti(token, U"^SET$") && current_sql_stmt == U"UPDATE") {
                    if (!o.wrap_after) new_line_();
                    over();
                } else if (!is_in_over && !is_in_filter && (!ti(token, U"^SET$") || current_sql_stmt == U"UPDATE")) {
                    if (has_next() && !t(nt(), UR"~(\(|;)~") && (!ti(nt(), U"^(UPDATE|KEY|NO)$") || uc(token) == U"WHERE")) {
                        if (!o.wrap_after) new_line_();
                        over();
                    }
                }
            }
            // Add newline before INSERT and DELETE if last token was AS (prepared statement)
            else if (ti(token, U"^(?:INSERT|DELETE|UPDATE)$") && uc(last) == U"AS") {
                new_line_();
                add_token(token);
            } else if (!t(current_sql_stmt, U"^(GRANT|REVOKE)$") && ti(token, U"^(?:SELECT|PERFORM|UPDATE|DELETE)$") &&
                       (!is_in_policy || o.format_type)) {
                no_break = 0;
                if (ti(token, U"^(SELECT|UPDATE|DELETE|INSERT)$") && is_in_policy) over();
                if (ti(token, U"^(UPDATE|DELETE|INSERT)$") && is_in_merge) {
                    if (!level_stack.empty() && level_stack.back() + 1 != level) over();
                }
                // case of ON DELETE/UPDATE clause in create table statements
                if (ti(token, U"^(UPDATE|DELETE)$") && is_in_create) {
                    add_token(token);
                    last = token;
                    continue;
                }
                if (ti(token, U"^UPDATE$") && ti(last, U"^(FOR|KEY|DO)$")) {
                    add_token(token);
                    if (uc(last) == U"DO" && uc(nt()) == U"SET") current_sql_stmt = U"DO UPDATE";
                } else if (!is_in_policy && !ti(token, U"^(DELETE|UPDATE)$") && (!has_next() || !ti(nt(), U"^DISTINCT$"))) {
                    if (last != U"\\\\") new_line_();
                    add_token(token);
                    if (!o.wrap_after && last != U"\\\\") new_line_();
                    over();
                } else {
                    if (is_in_policy > 1) new_line_();
                    add_token(token);
                    if (is_in_policy > 1) {
                        new_line_();
                        over();
                    }
                    if (is_in_policy) ++is_in_policy;
                }
            } else if (!t(current_sql_stmt, U"^(GRANT|REVOKE)$") && uc(token) == U"INSERT" && is_in_policy && o.format_type) {
                add_token(token);
                new_line_();
                over();
            } else if (ti(token, U"^(?:WITHIN)$")) {
                is_in_within = 1;
                has_order_by = 1;
                add_token(token);
                last = token;
                continue;
            } else if (ti(token, U"^(?:GROUP|ORDER|LIMIT|EXCEPTION)$") || (uc(token) == U"ON" && uc(nt()) == U"CONFLICT")) {
                if (o.format_type && uc(token) == U"GROUP" && uc(nt()) == U"BY") no_break = 1;
                is_in_order_by = uc(token) == U"ORDER" && uc(nt()) == U"BY" ? 1 : 0;
                is_in_value = 0;
                parenthesis_level_value = 0;
                if (uc(last) == U"EXCLUDE") { // (GROUP without a last token can't happen: last is always set)
                    add_token(token);
                    last = token;
                    continue;
                }
                if ((is_in_within && uc(token) == U"GROUP") || (is_in_over && uc(token) == U"ORDER")) {
                    add_token(token);
                    last = token;
                    continue;
                }
                if (has_over_in_join && uc(token) == U"GROUP") {
                    back();
                    has_over_in_join = 0;
                }
                is_in_join = 0;
                if (uc(token) == U"LIMIT") has_limit = 1;
                if (!ti(token, U"^EXCEPTION")) back();
                else set_level(pop_level());
                if (uc(token) != U"EXCEPTION" || uc(last) != U"RAISE") {
                    // Excluding CREATE/DROP GROUP
                    if (uc(token) != U"LIMIT" || !is_in_create) {
                        if (!is_in_function && !t(last, U"^(CREATE|DROP)$")) new_line_();
                    }
                }

                // Store current indent position to print END at the right level
                if (uc(last) != U"RAISE" && ti(token, U"^EXCEPTION$")) {
                    is_in_exception = 1;
                    if (!begin_level.empty()) set_level(begin_level.back());
                } else if (uc(last) == U"RAISE" && ti(token, U"^EXCEPTION$")) {
                    push_level(level);
                    over();
                }
                if (is_in_where) --is_in_where;
                add_token(token);
                if (ti(token, U"^EXCEPTION$") && level == 0) over();
            } else if (ti(token, U"^(?:BY)$") && !ti(last, U"^(?:INCREMENT|OWNED|PARTITION|GENERATED)$")) {
                add_token(token);
                if (ti(last, U"^(?:GROUP|ORDER)")) col_count = 0;
                if (!has_order_by && !is_in_over) {
                    if (!o.wrap_after && !is_in_function && !is_in_merge) new_line_();
                    over();
                }
            } else if (ti(token, U"^(?:CASE)$") && uc(last) != U"END") {
                if (is_in_policy) {
                    new_line_();
                    over();
                    add_token(token);
                    ++is_in_policy;
                } else {
                    add_token(token);
                }
                // Store current indent position to print END at the right level
                push_level(level);
                // Mark next WHEN statement as first element of a case to force indentation only
                // after this element
                first_when_in_case = 1;
                is_in_case.push_back(level);
            } else if (ti(token, U"^(?:WHEN)$") && is_keyword(token, next_token(), last)) {
                if (is_in_merge) {
                    set_level(level_stack.empty() ? std::nullopt : std::optional<long>{level_stack.back()});
                    if (is_in_merge == 1) push_level(level);
                    ++is_in_merge;
                }
                if (!first_when_in_case && !is_in_trigger && !is_in_merge && uc(last) != U"CASE") {
                    if (!is_in_exception) {
                        if (!level_stack.empty()) set_level(level_stack.back());
                    } else if (!begin_level.empty()) {
                        set_level(begin_level.back() + 1);
                    }
                }
                if (!ti(last, UR"~(^(CASE|,|\()$)~")) new_line_();
                add_token(token);
                if (is_in_case.empty() && !is_in_merge && !is_in_trigger) over();
                first_when_in_case = 0;
            } else if (ti(token, U"^(?:IF|LOOP)$") && current_sql_stmt != U"GRANT") {
                if (is_in_join) {
                    is_in_join = 0;
                    back();
                }
                add_token(token);
                no_break = 0;
                if (has_next() && !ti(nt(), U"^(EXISTS|;)$")) {
                    if (uc(nt()) != U"NOT" || uc(tk(1)) != U"EXISTS") {
                        if (ti(token, U"^LOOP$")) new_line_();
                        over();
                        push_level(level);
                        if (ti(token, U"^IF$")) is_in_if = 1;
                    }
                }
            } else if (ti(token, U"^THEN$")) {
                add_token(token);
                new_line_();
                if (is_in_if && !level_stack.empty()) set_level(level_stack.back());
                if (!is_in_case.empty() && has_next() && !ti(nt(), UR"~(^(\(|RAISE)$)~")) {
                    if (!level_stack.empty()) set_level(level_stack.back());
                    over();
                }
                if (!is_in_case.empty() && has_next() && nt() == U"(" && !ti(tk(1), U"^(SELECT|CASE)$")) {
                    if (!level_stack.empty()) set_level(level_stack.back());
                    over();
                }
                is_in_if = 0;
            } else if (ti(token, U"^(?:ELSE|ELSIF)$")) {
                if (is_in_case.empty()) back();
                else set_level(is_in_case.back());
                new_line_();
                add_token(token);
                if (!ti(token, U"^ELSIF$")) new_line_();
                over();
            } else if (ti(token, U"^(?:END)$")) {
                first_when_in_case = 0;
                if (!is_in_case.empty()) {
                    is_in_case.pop_back();
                    back();
                    set_level(pop_level());
                    if (!is_in_create_function || nt() == U";") {
                        new_line_();
                        add_token(token);
                        last = token;
                        continue;
                    }
                }
                // When we are not in a function code block (0 is the main begin/end block of a function)
                else if (is_in_block == -1 && last != U",") {
                    // END is closing a create function statement so reset position to begining
                    if (!ti(nt(), U"^(IF|LOOP|CASE|INTO|FROM|END|ELSE|AND|OR|WHEN|AS|,)$")) reset_level();
                    // otherwise back to last level stored at CASE keyword
                    else set_level(pop_level());
                }
                // We reach the last end of the code
                else if (is_in_block > -1 && t(nt(), UR"~(^(;|\$.*\$)$)~") && !is_in_exception) {
                    if (is_in_block == 0) {
                        reset_level();
                    } else {
                        set_level(pop_level() - 1);
                        --is_in_block;
                    }
                }
                // We are in code block
                else if (last != U",") {
                    // decrease the block level if this is a END closing a BEGIN block
                    if (!ti(nt(), U"^(IF|LOOP|CASE|INTO|FROM|END|ELSE|AND|OR|WHEN|AS|,)$")) --is_in_block;
                    // Go back to level stored with IF/LOOP/BEGIN/EXCEPTION block
                    if (is_in_block > -1) set_level(pop_level());
                    else reset_level();
                }
                if (nt() == U";") {
                    std::optional<long> position;
                    if (!begin_level.empty()) {
                        position = begin_level.back();
                        begin_level.pop_back();
                    }
                    set_level(position);
                } else if (!is_in_exception && !ti(nt(), U"^(AS|CASE|FROM|,)$")) {
                    if (ti(nt(), U"^(IF|LOOP|CASE|INTO|FROM|END|ELSE|AND|OR|WHEN|AS|,)$")) back();
                }
                new_line_();
                add_token(token);
            } else if (ti(token, UR"~(^(?:END::[^\s]+)$)~") && !is_in_case.empty()) {
                first_when_in_case = 0;
                is_in_case.pop_back();
                back();
                set_level(pop_level());
                new_line_();
                add_token(token);
            } else if (ti(token, U"^(?:UNION|INTERSECT|EXCEPT)$")) {
                no_break = 0;
                if (is_in_join) {
                    back();
                    is_in_join = 0;
                }
                if (is_in_return_query && !level_stack.empty()) set_level(level_stack.back());
                else if (last != U"(") back();
                new_line_();
                add_token(token);
                if (has_next() && nt() != U"(" && !ti(nt(), U"^ALL$")) new_line_();
                if (is_in_where) --is_in_where;
                is_in_from = 0;
            } else if (ti(token, U"^(?:LEFT|RIGHT|FULL|INNER|OUTER|CROSS|NATURAL)$") && uc(last) != U"MATCH") {
                no_break = 0;
                if (!is_in_join && last != U")") {
                    if (level_stack.empty() || level > level_stack.back() + 1) back();
                }
                if (has_over_in_join) {
                    has_over_in_join = 0;
                    back();
                }
                if (ti(token, U"(?:LEFT|RIGHT|FULL|CROSS|NATURAL)$")) {
                    new_line_();
                    if (level == 0 || (is_in_with > 1 && level == 1)) over();
                }
                if (ti(token, U"(?:INNER|OUTER)$") && !ti(last, U"(?:LEFT|RIGHT|CROSS|NATURAL|FULL)$")) {
                    new_line_();
                    if (!is_in_join) over();
                }
                add_token(token);
            } else if (ti(token, U"^(?:JOIN)$") && !is_in_operator) {
                no_break = 0;
                if (!ti(last, U"^(?:LEFT|RIGHT|FULL|INNER|OUTER|CROSS|NATURAL)$")) {
                    new_line_();
                    if (has_over_in_join) back();
                    has_over_in_join = 0;
                }
                add_token(token);
                is_in_join = 1;
            } else if (ti(token, U"^(?:AND|OR)$")) {
                is_in_where = 0;
                // Try to detect AND in BETWEEN clause to prevent newline insert
                if (uc(token) == U"AND" && (is_in_between || ti(last, U"^(PRECEDING|FOLLOWING|ROW)$"))) {
                    add_token(token);
                    last = token;
                    is_in_between = 0;
                    continue;
                }
                no_break = 0;
                if (is_in_join) {
                    over();
                    has_over_in_join = 1;
                }
                is_in_join = 0;
                if (!is_in_if && !is_in_index && !is_in_merge && !ti(last, U"^(?:CREATE)$") && (is_in_create <= 2 || is_in_policy) && !is_in_trigger) {
                    new_line_();
                    if (!and_level && (!level || is_in_alter)) over();
                    else if (and_level && !level && uc(token) == U"OR") over();
                    else if (!level_stack.empty() && level == level_stack.back()) over();
                }
                if (is_in_merge) over();
                add_token(token);
                if (!is_in_merge) ++and_level;
            } else if (test(token, UR"~(^\/\*.*\*\/$)~", "s")) {
                if (!o.no_comments) {
                    subst_all(token, re(UR"~(\n[\s\t]+\*)~", "gs"), with(U"\n*"));
                    if (!is_in_over && !is_in_function) {
                        if (last == U";") {
                            new_line_();
                            add_token(U"");
                        }
                        new_line_();
                    }
                    add_token(token);
                    if (o.spaces == 0) break_ = U"\n";
                    if (!is_in_function && !is_in_over && (!is_comment(token) || !has_next() || nt() != U")")) new_line_();
                    if (o.spaces == 0) break_ = U" ";
                }
            } else if (is_in_merge && ti(token, U"^(WHEN|USING)$")) {
                if (ti(token, U"^USING$")) over();
                new_line_();
                add_token(token);
                continue;
            } else if ((ti(token, U"^USING$") && !is_in_merge && !is_in_order_by && !is_in_exception &&
                        (current_sql_stmt != U"DELETE" || !ti(uc(nt()), UR"~(^(\(|LATERAL)$)~"))) ||
                       (uc(token) == U"WITH" && uc(nt()) == U"CHECK" && is_in_policy)) {
                if (!is_in_from) {
                    if (is_in_operator || (is_in_policy && !o.format_type && !is_in_policy && !is_in_using)) over();
                    if (is_in_policy && ti(token, U"^USING$")) {
                        policy_as_using = 1;
                        if (level == 0) over();
                    }
                    if (ti(token, U"^USING$")) push_level(level);
                    if (uc(token) == U"WITH" && uc(nt()) == U"CHECK" && is_in_policy && policy_as_using) set_level(pop_level());
                    if (uc(token) == U"WITH" && (is_in_policy > 1 || uc(nt()) == U"CHECK") && !o.format_type && is_in_using) set_level(pop_level());
                    if (uc(token) == U"WITH" && uc(nt()) == U"CHECK" && is_in_policy && level == 0) over();
                    if (uc(last) != U"EXCLUDE" && !is_in_index && !is_in_function) new_line_();
                } else {
                    // USING from join clause disable line break like in function
                    ++is_in_function;
                    // Restore FROM position
                    if (!is_in_join) set_level(pop_level());
                }
                add_token(token);

                if (has_next() && uc(nt()) == U"ERRCODE") set_level(pop_level());

                is_in_using = 1;
                if (!is_in_from && !is_in_join && uc(last) != U"EXCLUDE" && !is_in_function && !is_in_operator && !is_in_create && !is_in_index)
                    ++is_in_policy;
            } else if (ti(token, U"^EXCLUDE$")) {
                if (!ti(last, U"^(FOLLOWING|ADD)$") || !ti(nt(), U"^USING$")) {
                    if (!ti(last, U"^(FOLLOWING|ADD)$")) new_line_();
                }
                add_token(token);
                is_in_using = 1;
            } else if (t(token, UR"~(^\\\S)~")) {
                // treat everything starting with a \ and at least one character as psql meta command.
                add_token(token);
                if (token != U"\\\\" && has_next() && nt() != U"\\\\") new_line_();
            } else if (ti(token, U"^(ADD|DROP)$") && (current_sql_stmt == U"SEQUENCE" || current_sql_stmt == U"ALTER")) {
                if (!ti(nt(), U"^(NOT|NULL|DEFAULT)$") && (!is_in_alter || last != U"(")) {
                    new_line_();
                    if (is_in_alter < 2) over();
                }
                add_token(token, last);
                if (is_in_alter == 1) ++is_in_alter;
            } else if (ti(token, U"^INCREMENT$") && current_sql_stmt == U"SEQUENCE") {
                new_line_();
                add_token(token);
            } else if (ti(token, U"^NO$") && ti(nt(), U"^(MINVALUE|MAXVALUE)$")) {
                new_line_();
                add_token(token);
            } else if (!ti(last, UR"~(^(\(|NO)$)~") && ti(token, U"^(MINVALUE|MAXVALUE)$")) {
                new_line_();
                add_token(token);
            } else if (ti(token, U"^CACHE$")) {
                new_line_();
                add_token(token);
            } else {
                if (o.keep_newline && t(token, UR"~(^\s+$)~")) continue;

                if (is_in_returning && ti(token, U"^INSERT$")) is_in_returning = 0;

                if (truthy(fct_code_delimiter) && t(fct_code_delimiter, U"^'.*'$")) {
                    fct_code_delimiter.clear();
                    language_sql = 0;
                }
                if (is_in_block != -1 && !truthy(fct_code_delimiter)) {
                    is_in_block = -1;
                    is_in_procedure = 0;
                    is_in_function = 0;
                }

                // special case with comment
                if (test(token, UR"~((?:\s*--)[\ \t\S]*)~", "s")) {
                    if (!o.no_comments) {
                        match m;
                        const bool moved = subst(token, re(UR"~(^(\s*)(--.*))~", "s"), with(U"$2"), &m);
                        const str start = moved ? m[1] : str{};
                        if (test(start, U"\\n", "s")) {
                            if (last == U";" && !test(content_, U"\\n$", "s")) {
                                new_line_();
                                add_token(U"");
                            }
                            new_line_();
                        }
                        subst(token, re(UR"~(\s+$)~", "s"), with(U""));
                        subst(token, re(UR"~(^\s+)~", "s"), with(U""));
                        add_token(token);
                        if (truthy(start) || (!test(content_, U"\\n$", "s") && has_next() && uc(nt()) != U"AS")) new_line_();

                        // Add extra newline after the last comment if we are not in a block or a statement
                        if (has_next() && !t(nt(), UR"~(^\s*--)~")) {
                            if (is_in_block == -1 && !is_in_declare && !truthy(fct_code_delimiter) && !truthy(current_sql_stmt) &&
                                is_comment(last) && !test(content_, U"\\n$", "s"))
                                content_ += U"\n";
                        }
                        last = token;
                    }
                    continue;
                }

                if (ti(last, U"^(?:SEQUENCE)$") && !ti(nt(), U"^(OWNED|;)$")) {
                    add_token(token);
                    new_line_();
                    over();
                } else {
                    if (last == U")" && (!has_next() || nt() != U";")) {
                        if (!parenthesis_level && !has_over_in_join && is_in_from) {
                            long position = 0;
                            if (!level_parenthesis.empty()) {
                                position = level_parenthesis.back();
                                level_parenthesis.pop_back();
                            }
                            set_level(position ? position : 1);
                        }
                    }
                    if (uc(last) == U"UPDATE" && current_sql_stmt == U"UPDATE") {
                        new_line_();
                        over();
                    }
                    if (uc(last) == U"AS" && uc(token) == U"WITH") new_line_();

                    if (uc(token) == U"INSERT" && last == U";") {
                        if (!level_stack.empty()) {
                            set_level(level_stack.back());
                        } else {
                            if (!is_in_procedure || level > 1) back();
                        }
                    }

                    if ((is_in_policy > 1 || (is_in_policy && is_in_sub_query)) && ti(token, U"^(ALL|SELECT|UPDATE|DELETE|INSERT)$")) {
                        new_line_();
                        over();
                        add_token(token);
                        new_line_();
                        over();
                        last = token;
                        continue;
                    }
                    if (ti(token, U"^SELECT$") && is_in_policy) ++is_in_policy;
                    if (o.comma_break && current_sql_stmt == U"INSERT" && last == U"(") new_line_();

                    // Remove extra newline at end of code of SQL functions
                    if (token == U"'" && last == U";" && ti(nt(), U"^(;|LANGUAGE|STRICT|SET|IMMUTABLE|STABLE|VOLATILE)$"))
                        subst(content_, re(UR"~(\s+$)~", "s"), with(U"\n"));

                    if (!is_in_case.empty() && last == U";") new_line_();

                    if (ti(token, U"^(UPDATE|DELETE|INSERT)$") && is_in_merge) {
                        if ((level_stack.empty() ? 0 : level_stack.back()) + 1 != level) over();
                    }

                    if (t(current_sql_stmt, U"(SELECT|UPDATE)") && ti(token, U"^INTO$")) {
                        back();
                        new_line_();
                        // Finally add the token without further condition
                        add_token(token, last);
                        new_line_();
                        over();
                        continue;
                    }

                    if (is_in_returning && ti(token, U"^INTO$")) {
                        is_in_returning = 0;
                        back();
                        new_line_();
                        // Finally add the token without further condition
                        add_token(token, last);
                        new_line_();
                        over();
                        continue;
                    }

                    // Finally add the token without further condition
                    add_token(token, last);

                    if (uc(token) == U"RETURN" && ti(nt(), U"^QUERY$")) {
                        push_level(level);
                        is_in_return_query = 1;
                    }

                    if (last == U"'" && ti(token, U"^(BEGIN|DECLARE)$")) {
                        last = token;
                        new_line_();
                        over();
                    }

                    // Reset CREATE statement flag when using CTE
                    if (is_in_create && is_in_with && uc(token) == U"WITH" && uc(last) == U"AS") is_in_create = 0;
                    if (uc(last) == U"LANGUAGE" && is_keyword(last, token) && (!has_next() || nt() != U";")) new_line_();
                }
            }

            last = token;
            ++pos;
        }

        if (o.no_extra_line) {
            if (!test(content_, U";$", "s")) new_line_();
            subst(content_, re(UR"~(\s+$)~", "s"), with(U"\n"));
        } else {
            new_line_();
        }

        // Attempt to remove usless spaces
        subst_all(content_, re(UR"~(\s+CHECK\s+\(\s+)~", "igs"), with(U" CHECK ("));

        // Attempt to eliminate redundant parenthesis in DML queries: ((x)) becomes (x), but the two
        // parentheses of a scalar sub-SELECT stay when the outer one belongs to a function call or to
        // a VALUES row constructor.
        if (!o.redundant_parenthesis) {
            const auto &pattern = re(UR"~(
                                ((?:^|\s+)(?:WHERE|SELECT|FROM)\s+(?!TO)\s+[^;]+)  # $1
                                [\(]{2}([^\(\)]+)[\)]{2}([^;]+)                    # $2 inner, $3 rest
                        )~", "isx");
            while (subst_all(content_, pattern, [&](const match &x) {
                const str &lead = x[1], &inner = x[2], &rest = x[3];
                str word;
                if (const auto wm = find(lead, UR"~(([A-Za-z_][\w\.]*)\s*$)~")) word = wm[1];
                if (t(inner, UR"~(AAKEYWCONST\d+AA)~") ||
                    (ti(inner, UR"~(^\s*SELECT\b)~") && (ti(word, U"^VALUES$") || (!word.empty() && is_function(word)))))
                    return lead + U"\x02" + inner + U"\x03" + rest;
                return lead + U"(" + inner + U")" + rest;
            })) {}
            subst_all(content_, re(U"\\x02", "gs"), with(U"(("));
            subst_all(content_, re(U"\\x03", "gs"), with(U"))"));
        }

        // Add newline after ariga/atlas related comments
        subst_all(content_, re(UR"~((\-\-\s*atlas:[^\n]+))~", "gs"), with(U"$1\n"));
    }
}
