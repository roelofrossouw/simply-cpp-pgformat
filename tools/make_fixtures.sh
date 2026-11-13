#!/bin/bash
# Makes the test fixtures from a pgFormatter checkout (git clone --branch v5.11
# https://github.com/darold/pgFormatter): its test inputs, and what its own pg_format prints for
# them with the options its regression tests use (t/02_regress.t), into tests/resource. The C++
# test (tests/pgformat.cpp) uses the same options per file and must print the same bytes.
# Usage: tools/make_fixtures.sh PGFORMATTER_DIR
set -euo pipefail
src="$(cd "$1" && pwd)"
dir="$(cd "$(dirname "$0")/.." && pwd)/tests/resource"
rm -rf "$dir/sql" "$dir/expected"
mkdir -p "$dir/sql" "$dir/expected"
lst=$(mktemp)
trap 'rm -f "$lst"' EXIT
printf 'fct1\nMyFunction\n' > "$lst"
cp "$src/t/redshift.kw" "$dir/redshift.kw"

for f in "$src"/t/test-files/ex*.sql; do
    name=$(basename "$f")
    opt=()
    case "$name" in
        ex19.sql) opt=(-S '$f$') ;;
        ex46.sql) opt=(-W 4) ;;
        ex51.sql) opt=(-T -n) ;;
        ex64.sql) opt=(-k) ;;
        ex60.sql) opt=(-f 2 -u 2 -U 2) ;;
        ex57.sql) opt=(--comma-break -U 2) ;;
        ex67.sql) opt=(--anonymize) ;;
        ex68.sql) opt=(--nocomment) ;;
        ex69.sql) opt=(--extra-keyword "$src/t/redshift.kw") ;;
        ex62.sql) opt=(-w 60 -C -p 'https?:\/\/(www\.)?[-a-zA-Z0-9@:%._\+~#=]{1,256}\.[a-zA-Z0-9()]{1,6}\b([-a-zA-Z0-9()@:%_\+.~#?&//=]*)') ;;
        ex58.sql) opt=(--keyword-case 2 --function-case 1 --comma-start --wrap-after 1 --wrap-limit 40 --tabs --spaces 4) ;;
        ex70.sql) opt=(--no-space-function) ;;
        ex71.sql) opt=(--keyword-case 1 --type-case 1) ;;
        ex81.sql) opt=(--vertical-align --no-extra-line) ;;
        ex61.sql) opt=(--extra-function "$lst") ;;
    esac
    cp "$f" "$dir/sql/$name"
    (cd "$src" && perl ./pg_format ${opt[@]+"${opt[@]}"} -u 2 -X "$f") > "$dir/expected/$name"
done
for f in "$src"/t/pg-test-files/sql/*.sql; do
    name="pg-$(basename "$f")"
    cp "$f" "$dir/sql/$name"
    (cd "$src" && perl ./pg_format -u 2 -X "$f") > "$dir/expected/$name"
done
echo "$(ls "$dir/sql" | wc -l | tr -d ' ') fixtures in $dir"
