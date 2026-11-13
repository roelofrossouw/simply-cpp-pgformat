// sc-pgformat-demo: a query formatted as pg_format does by default, then with lower-case keywords,
// two spaces a level and the commas leading.

#include <sc_pgformat.h>

#include <iostream>

int main() {
    const char *sql = "select a.id,a.name,count(*) from accounts a join orders o on o.account_id=a.id "
                      "where o.created_at>now()-interval '30 days' group by a.id,a.name order by 3 desc;";
    std::cout << sc::pgformat::format(sql);

    sc::pgformat::options settings;
    settings.keyword_case = sc::pgformat::letter_case::lower;
    settings.spaces = 2;
    settings.comma = sc::pgformat::comma_position::start;
    std::cout << "\n" << sc::pgformat::format(sql, settings);
}
