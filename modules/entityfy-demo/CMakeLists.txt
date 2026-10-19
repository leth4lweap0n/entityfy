# entityfy demo: interactive console, comparison benchmark and migration probe.

add_executable(entityfy_console main.cpp)
target_link_libraries(entityfy_console PRIVATE entityfy::core)
target_compile_options(entityfy_console
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
)

add_executable(entityfy_bench bench_compare.cpp)
target_link_libraries(entityfy_bench PRIVATE entityfy::core entityfy::sqlite)
target_compile_options(entityfy_bench
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
)

add_executable(entityfy_migration_probe migration_probe.cpp)
target_link_libraries(entityfy_migration_probe PRIVATE entityfy::core)
target_compile_options(entityfy_migration_probe
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
)

# PostgreSQL benchmark mode compares the raw driver through libpq.
find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
    target_link_libraries(entityfy_bench PRIVATE PostgreSQL::PostgreSQL)
    target_compile_definitions(entityfy_bench PRIVATE ENTITYFY_BENCH_POSTGRESQL=1)
endif()
