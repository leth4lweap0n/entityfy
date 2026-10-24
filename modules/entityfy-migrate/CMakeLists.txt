# entityfy migrate: schema snapshots, diffing, apply/rollback and SQL scripts.

set(entityfy_migrate_sources
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

if(ENTITYFY_ENABLE_MYSQL)
    list(APPEND entityfy_migrate_sources src/mysql_provider.cpp)
endif()

if(ENTITYFY_ENABLE_SQLSERVER)
    list(APPEND entityfy_migrate_sources src/sqlserver_provider.cpp)
endif()

add_library(entityfy_migrate STATIC ${entityfy_migrate_sources})
add_library(entityfy::migrate ALIAS entityfy_migrate)
set_target_properties(entityfy_migrate PROPERTIES EXPORT_NAME migrate)

target_include_directories(entityfy_migrate
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
)
target_link_libraries(entityfy_migrate PUBLIC entityfy::core)
target_include_directories(entityfy_migrate PRIVATE
    $<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>
)
target_compile_options(entityfy_migrate
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
)

if(ENTITYFY_ENABLE_SQLITE)
    target_link_libraries(entityfy_migrate PRIVATE entityfy::sqlite)
    target_compile_definitions(entityfy_migrate PUBLIC ENTITYFY_ENABLE_SQLITE)
endif()

if(ENTITYFY_ENABLE_MYSQL)
    target_link_libraries(entityfy_migrate PRIVATE
        $<BUILD_INTERFACE:entityfy::mysql>
        $<INSTALL_INTERFACE:entityfy::mysql>)
    target_compile_definitions(entityfy_migrate PUBLIC ENTITYFY_ENABLE_MYSQL)
endif()

if(ENTITYFY_ENABLE_SQLSERVER)
    target_link_libraries(entityfy_migrate PRIVATE ODBC::ODBC)
    target_compile_definitions(entityfy_migrate PUBLIC ENTITYFY_ENABLE_SQLSERVER)
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
    set(entityfy_migrate_test_sources
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

    if(ENTITYFY_ENABLE_MYSQL)
        list(APPEND entityfy_migrate_test_sources tests/test_mysql.cpp)
    endif()

    if(ENTITYFY_ENABLE_SQLSERVER)
        list(APPEND entityfy_migrate_test_sources tests/test_sqlserver.cpp)
    endif()

    add_executable(entityfy_migrate_tests ${entityfy_migrate_test_sources})
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
