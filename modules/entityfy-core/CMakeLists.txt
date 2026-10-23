# entityfy core: models, annotations, SQL mapping and the database backends.

set(core_sources
    src/entityfy.cpp
)

if(ENTITYFY_ENABLE_SQLITE)
    list(APPEND core_sources src/sqlite_backend.cpp)
endif()

if(ENTITYFY_ENABLE_POSTGRESQL)
    list(APPEND core_sources src/postgresql_backend.cpp)
endif()

if(ENTITYFY_ENABLE_MYSQL)
    list(APPEND core_sources src/mysql_backend.cpp)
endif()

if(ENTITYFY_ENABLE_SQLSERVER)
    list(APPEND core_sources src/sqlserver_backend.cpp)
endif()

add_library(entityfy_core STATIC ${core_sources})
add_library(entityfy::core ALIAS entityfy_core)

target_include_directories(entityfy_core
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
)
target_compile_features(entityfy_core PUBLIC cxx_std_26)
target_compile_options(entityfy_core
    PUBLIC -freflection
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
)

if(ENTITYFY_ENABLE_SQLITE)
    target_link_libraries(entityfy_core PUBLIC entityfy::sqlite)
    target_compile_definitions(entityfy_core PUBLIC ENTITYFY_ENABLE_SQLITE)
endif()

if(ENTITYFY_ENABLE_POSTGRESQL)
    target_link_libraries(entityfy_core PRIVATE taocpp::taopq)
    target_compile_definitions(entityfy_core PUBLIC ENTITYFY_ENABLE_POSTGRESQL)
endif()

if(ENTITYFY_ENABLE_MYSQL)
    target_link_libraries(entityfy_core PRIVATE entityfy::mysql)
    target_compile_definitions(entityfy_core PUBLIC ENTITYFY_ENABLE_MYSQL)
endif()

if(ENTITYFY_ENABLE_SQLSERVER)
    target_link_libraries(entityfy_core PRIVATE ODBC::ODBC)
    target_compile_definitions(entityfy_core PUBLIC ENTITYFY_ENABLE_SQLSERVER)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(ENTITYFY_BUILD_EXAMPLES)
    if(NOT ENTITYFY_ENABLE_SQLITE)
        message(FATAL_ERROR "ENTITYFY_BUILD_EXAMPLES requires ENTITYFY_ENABLE_SQLITE")
    endif()

    file(GLOB example_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")

    foreach(example_source IN LISTS example_sources)
        get_filename_component(example_name "${example_source}" NAME_WE)
        add_executable(entityfy_example_${example_name} "${example_source}")
        target_link_libraries(entityfy_example_${example_name} PRIVATE entityfy::core spdlog::spdlog)
        target_compile_options(entityfy_example_${example_name}
            PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
        )
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ENTITYFY_BUILD_TESTS)
    file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")

    if(NOT ENTITYFY_ENABLE_SQLITE)
        list(REMOVE_ITEM test_sources "${CMAKE_CURRENT_SOURCE_DIR}/tests/SqliteBackend.test.cpp")
    endif()
    if(NOT ENTITYFY_ENABLE_POSTGRESQL)
        list(REMOVE_ITEM test_sources "${CMAKE_CURRENT_SOURCE_DIR}/tests/PostgresqlBackend.test.cpp")
    endif()
    if(NOT ENTITYFY_ENABLE_MYSQL)
        list(REMOVE_ITEM test_sources "${CMAKE_CURRENT_SOURCE_DIR}/tests/MysqlBackend.test.cpp")
    endif()
    if(NOT ENTITYFY_ENABLE_SQLSERVER)
        list(REMOVE_ITEM test_sources "${CMAKE_CURRENT_SOURCE_DIR}/tests/SqlserverBackend.test.cpp")
    endif()

    if(test_sources)
        add_executable(entityfy_core_tests ${test_sources})
        target_link_libraries(entityfy_core_tests PRIVATE entityfy::core Catch2::Catch2WithMain)
        target_compile_options(entityfy_core_tests
            PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
        )
        include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
        catch_discover_tests(entityfy_core_tests)
    endif()
endif()
