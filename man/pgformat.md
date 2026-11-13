# EXAMPLES
```
pgformat query.sql
pgformat -u 1 -s 2 -o pretty.sql query.sql
psql -c '\sf my_function' | pgformat -
pgformat -i *.sql
```

# FILES
`./.pg_format`, `~/.pg_format` and `$XDG_CONFIG_HOME/pg_format/pg_format.conf`: options as `name = value` lines (such as `keyword-case = 1`), read when the command line doesn't give them, unless `--no-rcfile`.

# NOTES
pgformat follows pgFormatter 5.11's pg_format and gives the same output. Where pg_format stops with a Perl error (an undefined value), pgformat carries on. Negated options (`--no-anonymize` and the like) aren't taken; `--version` shows pgformat's version.

# AUTHORS
pgFormatter is Gilles Darold's (PostgreSQL Licence), with code from SQL::Beautify by Jonas Kramer (Artistic License 2.0). The port to C++ is Roelof Rossouw's.

# SEE ALSO
pg_format(1), psql(1)
