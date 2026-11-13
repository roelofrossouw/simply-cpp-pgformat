// pgformat, the program: pgFormatter's pg_format on sc::pgformat, with its options by the same
// letters and names, its configuration file (./.pg_format, ~/.pg_format or
// $XDG_CONFIG_HOME/pg_format/pg_format.conf) and its handling of several input files. Named
// pgformat so it can be installed beside pgFormatter's pg_format.

#include <sc_pgformat.h>

#include <params.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>

namespace {
    std::string read_all(std::istream &in) { return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}; }

    bool exists(const std::string &path) {
        std::error_code error;
        return std::filesystem::exists(path, error);
    }

    // A list file (--extra-function, --extra-keyword): a word a line; a line of only spaces,
    // commas and semicolons is skipped (pg_format splits each line on /^[\s,;]+$/).
    std::optional<std::vector<std::string>> read_list(const std::string &path) {
        std::ifstream in{path};
        if (!in) return std::nullopt;
        std::vector<std::string> words;
        std::string line;
        while (std::getline(in, line)) {
            if (line.find_first_not_of(" \t\r\n\f\v,;") == std::string::npos) continue;
            words.push_back(line);
        }
        return words;
    }

    // pg_format's configuration file: "name = value" lines that start with a lower-case letter.
    std::map<std::string, std::string> read_config(const std::string &path) {
        std::map<std::string, std::string> values;
        std::ifstream in{path};
        if (!in) {
            std::cerr << "ERROR: can not read file " << path << "\n";
            std::exit(1);
        }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] < 'a' || line[0] > 'z') continue;
            const auto name_end = line.find_first_of(" \t\r\n\f\v=");
            std::size_t at = name_end == std::string::npos ? line.size() : name_end;
            while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at]))) ++at;
            if (at >= line.size() || line[at] != '=') continue;
            ++at;
            while (at < line.size() && std::isspace(static_cast<unsigned char>(line[at]))) ++at;
            const auto value_end = line.find_first_of(" \t\r\n\f\v", at);
            const std::string value = line.substr(at, value_end == std::string::npos ? std::string::npos : value_end - at);
            if (value.empty()) continue;
            std::string name = line.substr(0, name_end);
            for (auto &c: name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (values.contains(name)) continue;
            values[name] = value;
        }
        return values;
    }

    // Perl's truth for a value from the configuration file.
    bool truthy(const std::string &value) { return !value.empty() && value != "0"; }

    sc::pgformat::letter_case letter_case(const std::string &name, const std::string &value) {
        if (value.size() != 1 || value[0] < '0' || value[0] > '3') {
            std::cerr << "Error: " << name << " can be only one of: 0, 1, 2, or 3.\n\nTry 'pgformat --help' for more information.\n";
            std::exit(2);
        }
        return static_cast<sc::pgformat::letter_case>(value[0] - '0');
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "pgformat"},
        {"description", "Formats PostgreSQL SQL queries and PL/pgSQL code, as pgFormatter's pg_format does (sc-pgformat is a "
                        "port of its Perl, with the same output), with its options by the same letters."},
        {"version", SC_PGFORMAT_VERSION},
        {"epilog", "FILE can be a file, several files, or - to read the SQL from standard input (the default). The formatted SQL "
                   "goes to standard output unless --output or --inplace says otherwise. Options not given are read from a "
                   "configuration file, as pg_format reads them: --config FILE, else ./.pg_format, ~/.pg_format or "
                   "$XDG_CONFIG_HOME/pg_format/pg_format.conf (unless --no-rcfile), with lines such as \"keyword-case = 1\". "
                   "pgFormatter is Gilles Darold's (PostgreSQL Licence)."},
        {"options", {
            {"anonymize", {{"short", "a"}, {"type", "boolean"}, {"help", "obscure all literals in queries, to hide confidential data before formatting"}}},
            {"comma-start", {{"short", "b"}, {"type", "boolean"}, {"help", "in a parameter list, start a line with the comma (see -e)"}}},
            {"comma-break", {{"short", "B"}, {"type", "boolean"}, {"help", "in an INSERT statement, add a newline after each comma"}}},
            {"config", {{"short", "c"}, {"value", "FILE"}, {"help", "read options from this configuration file"}}},
            {"wrap-comment", {{"short", "C"}, {"type", "boolean"}, {"help", "with --wrap-limit, wrap comments too"}}},
            {"debug", {{"short", "d"}, {"type", "boolean"}, {"help", "say what it does on standard error"}}},
            {"comma-end", {{"short", "e"}, {"type", "boolean"}, {"help", "in a parameter list, end a line with the comma (the default)"}}},
            {"function-case", {{"short", "f"}, {"value", "N"},
                               {"help", "the case of PostgreSQL functions: 0 unchanged (default), 1 lower, 2 upper, 3 capitalized"}}},
            {"format", {{"short", "F"}, {"value", "FORMAT"}, {"help", "output text (default) or html"}}},
            {"nogrouping", {{"short", "g"}, {"type", "boolean"}, {"help", "add a newline between statements in a transaction"}}},
            {"inplace", {{"short", "i"}, {"type", "boolean"}, {"help", "write the formatted SQL over the input files"}}},
            {"keep-newline", {{"short", "k"}, {"type", "boolean"}, {"help", "keep empty lines in PL/pgSQL code"}}},
            {"no-extra-line", {{"short", "L"}, {"type", "boolean"}, {"help", "no extra empty line at the end of the output"}}},
            {"maxlength", {{"short", "m"}, {"value", "SIZE"}, {"help", "cut queries at this many characters"}}},
            {"multiline", {{"short", "M"}, {"type", "boolean"}, {"help", "with --placeholder, . matches newlines too"}}},
            {"nocomment", {{"short", "n"}, {"type", "boolean"}, {"help", "remove comments"}}},
            {"numbering", {{"short", "N"}, {"type", "boolean"}, {"help", "a comment numbering each statement"}}},
            {"output", {{"short", "o"}, {"value", "FILE"}, {"help", "write the formatted SQL to FILE (default: standard output)"}}},
            {"placeholder", {{"short", "p"}, {"value", "REGEX"}, {"help", "a regular expression for code that must not be changed"}}},
            {"redshift", {{"short", "r"}, {"type", "boolean"}, {"help", "add Amazon Redshift's keywords (as --extra-keyword redshift)"}}},
            {"spaces", {{"short", "s"}, {"value", "SIZE"}, {"help", "spaces a level of indentation (default 4)"}}},
            {"separator", {{"short", "S"}, {"value", "STRING"}, {"help", "the dynamic code separator (default: a single quote)"}}},
            {"format-type", {{"short", "t"}, {"type", "boolean"}, {"help", "another layout for some statements"}}},
            {"tabs", {{"short", "T"}, {"type", "boolean"}, {"help", "indent with tabs, one a level"}}},
            {"keyword-case", {{"short", "u"}, {"value", "N"},
                              {"help", "the case of keywords: 0 unchanged, 1 lower, 2 upper (default), 3 capitalized"}}},
            {"type-case", {{"short", "U"}, {"value", "N"},
                           {"help", "the case of data types: 0 unchanged, 1 lower (default), 2 upper, 3 capitalized"}}},
            {"wrap-limit", {{"short", "w"}, {"value", "N"}, {"help", "wrap lines longer than N characters"}}},
            {"wrap-after", {{"short", "W"}, {"value", "N"}, {"help", "items a line in lists (default: one a line)"}}},
            {"no-rcfile", {{"short", "X"}, {"type", "boolean"}, {"help", "don't read a configuration file unless --config names one"}}},
            {"extra-function", {{"value", "FILE"}, {"help", "a file of function names to format as PostgreSQL's functions"}}},
            {"extra-keyword", {{"value", "FILE"},
                               {"help", "a file of keywords to format as PostgreSQL's keywords; redshift for Amazon Redshift's"}}},
            {"no-space-function", {{"type", "boolean"}, {"help", "no space between a function call and its parenthesis"}}},
            {"redundant-parenthesis", {{"type", "boolean"}, {"help", "keep redundant parentheses in DML"}}},
            {"vertical-align", {{"type", "boolean"}, {"help", "line up CREATE TABLE column definitions and trailing comments"}}}}},
        {"extra", {{"files", {{"help", "the SQL files (- for standard input)"}}}}}}};

    // The options given, as text like the configuration file's (flags "1" when given).
    std::map<std::string, std::string> cfg;
    for (const auto &[name, spec]: args.structure()["options"].items()) {
        const auto *value = args.lookup(name);
        if (!value || value->is_null()) continue;
        if (value->is_boolean()) {
            if (value->get<bool>()) cfg[name] = "1";
        } else {
            cfg[name] = value->is_string() ? value->get<std::string>() : value->dump();
        }
    }

    const auto given = [&](const std::string &name) { return cfg.contains(name); };
    if (!given("no-rcfile")) {
        std::optional<std::string> found;
        const char *home = std::getenv("HOME");
        const char *profile = std::getenv("USERPROFILE");
        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        if (exists(".pg_format")) found = ".pg_format";
        else if (home && exists(std::string{home} + "/.pg_format")) found = std::string{home} + "/.pg_format";
        else if (profile && exists(std::string{profile} + "/.pg_format")) found = std::string{profile} + "/.pg_format";
        else if (xdg && exists(std::string{xdg} + "/pg_format/pg_format.conf")) found = std::string{xdg} + "/pg_format/pg_format.conf";
        if (found && !given("config")) cfg["config"] = *found;
    }
    if (given("config") && std::filesystem::is_regular_file(cfg["config"])) {
        for (auto [name, value]: read_config(cfg["config"])) {
            // do not override command line arguments
            if (given(name)) continue;
            if (name == "comma" || name == "format")
                for (auto &c: value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            cfg[name] = value;
        }
    }

    const auto value = [&](const std::string &name, const std::string &fallback) { return given(name) ? cfg[name] : fallback; };
    const auto flag = [&](const std::string &name) { return given(name) && truthy(cfg[name]); };
    const auto number = [&](const std::string &name, const long fallback) {
        if (!given(name)) return fallback;
        try {
            return std::stol(cfg[name]);
        } catch (const std::exception &) {
            std::cerr << "pgformat: " << name << ": not a number: " << cfg[name] << "\n";
            std::exit(2);
        }
    };

    sc::pgformat::options settings;
    settings.spaces = number("spaces", 4);
    settings.tabs = flag("tabs");
    settings.comma_break = flag("comma-break");
    settings.maxlength = static_cast<std::size_t>(std::max(0L, number("maxlength", 0)));
    settings.format_type = flag("format-type");
    settings.wrap_limit = number("wrap-limit", 0);
    settings.wrap_after = number("wrap-after", 0);
    settings.wrap_comment = flag("wrap-comment");
    settings.numbering = flag("numbering");
    settings.no_extra_line = flag("no-extra-line");
    settings.keep_newline = flag("keep-newline");
    settings.no_comments = flag("nocomment");
    settings.no_grouping = flag("nogrouping");
    settings.anonymize = flag("anonymize");
    settings.placeholder = value("placeholder", "");
    settings.multiline = flag("multiline");
    settings.separator = value("separator", "");
    settings.no_space_function = flag("no-space-function");
    settings.redundant_parenthesis = flag("redundant-parenthesis");
    settings.vertical_align = flag("vertical-align");

    std::string comma = value("comma", "end");
    for (auto &c: comma) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (comma != "end" && comma != "start") {
        std::cout << "FATAL: unknown value for comma: " << comma << "\n";
        return 0;
    }
    std::string format = value("format", "text");
    for (auto &c: format) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (format != "text" && format != "html") {
        std::cout << "FATAL: unknown output format: " << format << "\n";
        return 0;
    }
    settings.format = format == "html" ? sc::pgformat::output_format::html : sc::pgformat::output_format::text;

    const std::string extra_function = value("extra-function", "");
    if (!extra_function.empty() && !exists(extra_function)) {
        std::cout << "FATAL: file for extra function list does not exists: " << extra_function << "\n";
        return 0;
    }
    std::string extra_keyword = value("extra-keyword", "");
    if (flag("redshift")) extra_keyword = "redshift";
    if (!extra_keyword.empty() && extra_keyword != "redshift" && !exists(extra_keyword)) {
        std::cout << "FATAL: file for extra keyword list does not exists: " << extra_keyword << "\n";
        return 0;
    }
    if (truthy(extra_function)) {
        if (const auto words = read_list(extra_function)) settings.extra_functions = *words;
        else std::cerr << "WARNING: can not read file " << extra_function << "\n";
    }
    if (truthy(extra_keyword) && extra_keyword != "redshift") {
        if (const auto words = read_list(extra_keyword)) settings.extra_keywords = *words;
        else std::cerr << "WARNING: can not read file " << extra_keyword << "\n";
    } else if (extra_keyword == "redshift") {
        settings.redshift = true;
    }

    if (flag("inplace") && given("output")) {
        std::cerr << "Error: can not use -i | --inplace option together with the -o | --output option.\n\n"
                     "Try 'pgformat --help' for more information.\n";
        return 2;
    }

    const bool debug = flag("debug");
    const auto log = [&](const std::string &message) {
        if (debug) std::cerr << "DEBUG: " << message << "\n";
    };

    std::vector<std::string> inputs;
    for (const auto &file: args["files"]) inputs.push_back(file.get<std::string>());
    if (inputs.empty()) inputs.emplace_back("-");

    std::string output = value("output", "");
    for (const auto &input: inputs) {
        if (output.empty()) output = "-";
        settings.function_case = letter_case("function-case", value("function-case", "0"));
        settings.keyword_case = letter_case("keyword-case", value("keyword-case", "2"));
        settings.type_case = letter_case("type-case", value("type-case", "1"));
        // Force output file to be the same as input file when the inplace option is used
        if (flag("inplace")) output = input;
        if (flag("comma-end")) comma = "end";
        else if (flag("comma-start")) comma = "start";
        settings.comma = comma == "start" ? sc::pgformat::comma_position::start : sc::pgformat::comma_position::end;

        log("Starting to parse SQL file: " + input);
        std::string sql;
        if (input == "-") {
            sql = read_all(std::cin);
        } else {
            std::ifstream in{input, std::ios::binary};
            if (!in) {
                std::cerr << "pgformat: can't open '" << input << "' for reading\n";
                return 1;
            }
            sql = read_all(in);
        }
        log("Beautifying");
        std::string ready;
        try {
            ready = sc::pgformat::format(sql, settings);
        } catch (const std::exception &e) {
            std::cerr << "pgformat: " << e.what() << "\n";
            return 1;
        }
        if (output != "-") {
            log("Formatted SQL queries will be written to file " + output);
            std::ofstream out{output, std::ios::binary};
            if (!out) {
                std::cerr << "pgformat: can't open '" << output << "' for writing\n";
                return 1;
            }
            out << ready;
        } else {
            log("Formatted SQL queries will be written to stdout");
            std::cout << ready;
        }
    }
    return 0;
}
