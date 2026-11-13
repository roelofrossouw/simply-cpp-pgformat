#pragma once
// pgFormatter's dictionaries (set_dicts), generated into dicts.cpp by tools/dump_dicts.pl.

#include <string_view>
#include <utility>
#include <vector>

namespace sc::pgformat::detail {
    const std::vector<std::string_view> &pg_keywords();
    const std::vector<std::string_view> &pg_types();
    const std::vector<std::string_view> &sql_keywords();
    const std::vector<std::string_view> &redshift_keywords();
    const std::vector<std::string_view> &pg_functions();
    const std::vector<std::string_view> &copy_keywords();
    const std::vector<std::string_view> &brackets();
    // HTML output's symbols: the operator and its HTML.
    const std::vector<std::pair<std::string_view, std::string_view>> &symbols();
}
