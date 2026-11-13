set(SC_TEST_LINK_LIBRARIES sc-pgformat)

# Every fixture in resource/ (pgFormatter's own test files, and what its pg_format prints for them,
# made by tools/make_fixtures.sh) formatted again, which must give the same bytes; and pgformat, the
# program, on a few of them.
add_sc_test(pgformat LABELS unit TIMEOUT 600)
target_compile_definitions(test-pgformat PRIVATE
        SC_PGFORMAT_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/resource"
        SC_PGFORMAT_PROGRAM="$<TARGET_FILE:pgformat>")
add_dependencies(test-pgformat pgformat)
