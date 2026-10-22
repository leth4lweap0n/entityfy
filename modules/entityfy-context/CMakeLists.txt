# entityfy context: DbContext style change tracking on top of entityfy core.

add_library(entityfy_context STATIC src/db_context.cpp)
add_library(entityfy::db_context ALIAS entityfy_context)

target_include_directories(entityfy_context
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
)
target_link_libraries(entityfy_context PUBLIC entityfy::core)
target_compile_options(entityfy_context
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(ENTITYFY_BUILD_TESTS)
    add_executable(entityfy_context_tests tests/test_main.cpp)
    target_link_libraries(entityfy_context_tests PRIVATE entityfy::db_context)
    target_compile_options(entityfy_context_tests
        PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-but-set-variable -Wno-unused-parameter
    )
    add_test(NAME entityfy_context_tests COMMAND entityfy_context_tests)
endif()
