// sc-pgformat against pgFormatter 5.11: every file in resource/sql formatted with the options
// pgFormatter's own regression tests use for it (t/02_regress.t; -u 2 always) must give exactly
// what pg_format printed (resource/expected, made by tools/make_fixtures.sh).

#include <sc_pgformat.h>

#include "sc_test.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {
    const std::filesystem::path resources{SC_PGFORMAT_RESOURCES};

    std::string read(const std::filesystem::path &path) {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    // t/02_regress.t's options for a file, before the -u 2 every run gets.
    sc::pgformat::options options_for(const std::string &name) {
        using namespace sc::pgformat;
        options o;
        if (name == "ex19.sql") o.separator = "$f$";
        else if (name == "ex46.sql") o.wrap_after = 4;
        else if (name == "ex51.sql") o.tabs = true, o.no_comments = true;
        else if (name == "ex64.sql") o.keep_newline = true;
        else if (name == "ex60.sql") o.function_case = letter_case::upper, o.type_case = letter_case::upper;
        else if (name == "ex57.sql") o.comma_break = true, o.type_case = letter_case::upper;
        else if (name == "ex67.sql") o.anonymize = true;
        else if (name == "ex68.sql") o.no_comments = true;
        else if (name == "ex69.sql") {
            std::istringstream words{read(resources / "redshift.kw")};
            for (std::string word; std::getline(words, word);)
                if (word.find_first_not_of(" \t\r\n,;") != std::string::npos) o.extra_keywords.push_back(word);
        } else if (name == "ex62.sql") {
            o.wrap_limit = 60;
            o.wrap_comment = true;
            o.placeholder = R"(https?:\/\/(www\.)?[-a-zA-Z0-9@:%._\+~#=]{1,256}\.[a-zA-Z0-9()]{1,6}\b([-a-zA-Z0-9()@:%_\+.~#?&//=]*))";
        } else if (name == "ex58.sql") {
            o.function_case = letter_case::lower;
            o.comma = comma_position::start;
            o.wrap_after = 1;
            o.wrap_limit = 40;
            o.tabs = true;
        } else if (name == "ex70.sql") o.no_space_function = true;
        else if (name == "ex71.sql") o.type_case = letter_case::lower; // its --keyword-case 1 is overridden by -u 2
        else if (name == "ex81.sql") o.vertical_align = true, o.no_extra_line = true;
        else if (name == "ex61.sql") o.extra_functions = {"fct1", "MyFunction"};
        o.keyword_case = letter_case::upper;
        return o;
    }

    // Where two texts first differ, as "line N: expected ... got ...".
    std::string first_difference(const std::string &expected, const std::string &got) {
        std::istringstream a{expected}, b{got};
        std::string la, lb;
        for (int line = 1;; ++line) {
            const bool ma = static_cast<bool>(std::getline(a, la)), mb = static_cast<bool>(std::getline(b, lb));
            if (!ma && !mb) return "they differ only in their last line's end";
            if (!ma || !mb || la != lb)
                return "line " + std::to_string(line) + ": expected [" + (ma ? la : "<end>") + "] got [" + (mb ? lb : "<end>") + "]";
        }
    }

    std::string run(const std::string &command) {
        std::string out;
        if (FILE *pipe = popen(command.c_str(), "r")) {
            char buffer[65536];
            std::size_t n;
            while ((n = fread(buffer, 1, sizeof buffer, pipe)) > 0) out.append(buffer, n);
            pclose(pipe);
        }
        return out;
    }
}

int main() {
    std::vector<std::filesystem::path> files;
    for (const auto &entry: std::filesystem::directory_iterator(resources / "sql")) files.push_back(entry.path());
    std::ranges::sort(files);

    SECTION("pgFormatter's test files, byte for byte");
    {
        int same = 0;
        for (const auto &file: files) {
            const auto name = file.filename().string();
            const auto expected = read(resources / "expected" / name);
            std::string got;
            try {
                got = sc::pgformat::format(read(file), options_for(name));
            } catch (const std::exception &e) {
                CHECK_MSG(false, name + ": " + e.what());
                continue;
            }
            if (name == "ex67.sql") {
                // Anonymized: random literals, so only checked for what must be gone.
                const bool hidden = got.find("confirmed") == std::string::npos && got.find("hello") == std::string::npos;
                CHECK_MSG(hidden, name + ": literals left in the anonymized query");
                same += hidden;
                continue;
            }
            if (got == expected) ++same;
            else CHECK_MSG(false, name + ": " + first_difference(expected, got));
        }
        std::printf("   %d of %zu files the same\n", same, files.size());
        CHECK_EQ(same, static_cast<int>(files.size()));
    }

    SECTION("pgformat, the program, gives the library's output");
    {
        const auto file = resources / "sql" / "ex1.sql";
        const std::string program = SC_PGFORMAT_PROGRAM;
        CHECK_EQ(run("'" + program + "' -X -u 2 '" + file.string() + "'"), read(resources / "expected" / "ex1.sql"));
        CHECK_EQ(run("'" + program + "' -X -u 2 - < '" + file.string() + "'"), read(resources / "expected" / "ex1.sql"));
    }

    SECTION("Options");
    {
        using namespace sc::pgformat;
        options lower;
        lower.keyword_case = letter_case::lower;
        lower.spaces = 2;
        CHECK_EQ(format("SELECT a,b FROM t WHERE c=1;", lower), std::string{"select\n  a,\n  b\nfrom\n  t\nwhere\n  c = 1;\n\n"});
        CHECK_EQ(pgformatter_version(), std::string_view{"5.11"});
    }

    TEST_SUMMARY();
}
