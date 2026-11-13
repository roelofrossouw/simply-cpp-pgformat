# simply-cpp-pgformat

`sc-pgformat` formats PostgreSQL SQL and PL/pgSQL as
[pgFormatter](https://github.com/darold/pgFormatter) 5.11 (Gilles Darold's `pg_format`)
does: it is a port of pgFormatter's Perl to C++, and gives the same output byte for byte. All of
pgFormatter's own test files (82 examples and 195 of PostgreSQL's regression scripts) come out
exactly as `pg_format` prints them, and it is about three times faster.

```cpp
#include <sc_pgformat.h>

std::string pretty = sc::pgformat::format("select a,b from t where c=1;");
// SELECT
//     a,
//     b
// FROM
//     t
// WHERE
//     c = 1;

sc::pgformat::options settings;
settings.keyword_case = sc::pgformat::letter_case::lower;   // -u 1
settings.spaces = 2;                                        // -s 2
settings.comma = sc::pgformat::comma_position::start;       // -b
settings.wrap_limit = 80;                                   // -w 80
pretty = sc::pgformat::format(sql, settings);
```

- Every `pg_format` option is in `sc::pgformat::options`, by the same name: letter cases for
  keywords, functions and types, indentation (spaces or tabs), comma placement, wrapping,
  statement numbering, comment removal, anonymization of literals, vertical alignment of
  CREATE TABLE columns, HTML output, placeholders for code to leave alone, and extra keywords or
  functions (Amazon Redshift's keywords built in).
- Text is UTF-8 and handled as characters, as Perl does: alignment and wrapping count
  characters, not bytes.
- Regular expressions run on PCRE2 (its 32-bit library), which matches as Perl does; the
  `placeholder` option is such an expression.
- Where `pg_format` stops with a Perl error (it treats an undefined value as fatal, for example
  after the last token), `sc-pgformat` carries on.

## The pgformat program

The package installs `pgformat` and its man page: `pg_format`'s command line, with its options by
the same letters and names, its configuration file (`./.pg_format`, `~/.pg_format` or
`$XDG_CONFIG_HOME/pg_format/pg_format.conf`, unless `-X`) and several input files:

```bash
pgformat query.sql                          # to standard output
pgformat -u 1 -s 2 -o pretty.sql query.sql  # lower-case keywords, two spaces a level
cat query.sql | pgformat -                  # standard input
pgformat -i *.sql                           # in place
```

It's named `pgformat` so it can be installed beside pgFormatter's `pg_format` (Ubuntu's
`pgformatter` package). `--version` shows sc-pgformat's version; negated options
(`--no-anonymize` and the like) aren't taken.

## Building

It needs sc-core (for the program) and PCRE2 (`libpcre2-dev`, Homebrew's `pcre2`). The tests
(`ctest`) format every file in `tests/resource/sql` and compare with `tests/resource/expected`,
which `tools/make_fixtures.sh` makes with pgFormatter's own `pg_format`; `tools/dump_dicts.pl`
regenerates `src/dicts.cpp` (the keyword, type and function lists) from pgFormatter.

## Licences

`sc-pgformat` is Apache-2.0. It is a port of pgFormatter (PostgreSQL Licence), which embeds
SQL::Beautify by Jonas Kramer (Artistic License 2.0), and of Perl's Text::Wrap; NOTICE has their
notices. The test files come from pgFormatter and PostgreSQL (PostgreSQL Licence).
