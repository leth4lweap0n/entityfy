# entityfy migrate: schema snapshots, diffing, apply/rollback and SQL scripts.

add_library(entityfy_migrate STATIC
    src/schema.cpp
    src/json.cpp
    src/util.cpp
    src/sha256.cpp
    src/diff.cpp
    src/engine.cpp
    src/scaffold.cpp
    src/table_ops.cpp
    src/sqlite_provider.cpp
    src/postgres_provider.cpp
    src/pq_dynamic.cpp
)
add_library(entityfy::migrate ALIAS entityfy_migrate)

target_include_directories(entityfy_migrate
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
)
target_link_libraries(entityfy_migrate
    PUBLIC entityfy::core
    PRIVATE nlohmann_json::nlohmann_json
)
target_compile_options(entityfy_migrate
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
)

if(ENTITYFY_ENABLE_SQLITE)
    target_link_libraries(entityfy_migrate PRIVATE entityfy::sqlite)
    target_compile_definitions(entityfy_migrate PUBLIC ENTITYFY_ENABLE_SQLITE)
endif()

# ---------------------------------------------------------------------------
# Command line interface
# ---------------------------------------------------------------------------
add_executable(entityfy_migrate_cli cli/main.cpp)
target_link_libraries(entityfy_migrate_cli PRIVATE entityfy::migrate)
target_include_directories(entityfy_migrate_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
target_compile_options(entityfy_migrate_cli
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ENTITYFY_BUILD_TESTS)
    add_executable(entityfy_migrate_tests
        tests/test_main.cpp
        tests/test_schema.cpp
        tests/test_diff.cpp
        tests/test_json.cpp
        tests/test_sqlite.cpp
        tests/test_engine.cpp
        tests/test_scaffold.cpp
        tests/test_postgres.cpp
        tests/test_table_ops.cpp
        tests/test_owned_model.cpp
    )
    target_link_libraries(entityfy_migrate_tests PRIVATE entityfy::migrate)
    target_include_directories(entityfy_migrate_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    target_compile_definitions(entityfy_migrate_tests PRIVATE
        ENTITYFY_CORE_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/../entityfy-core/include"
    )
    target_compile_options(entityfy_migrate_tests
        PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
    )
    add_test(NAME entityfy_migrate_tests COMMAND entityfy_migrate_tests)
endif()
