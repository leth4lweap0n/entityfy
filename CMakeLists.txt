cmake_minimum_required(VERSION 3.30)

project(entityfy
    VERSION 0.1.0
    DESCRIPTION "C++26 reflection based ORM toolkit: core, context tracking and schema migrations"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(ENTITYFY_BUILD_TESTS "Build entityfy test suites" ON)
option(ENTITYFY_BUILD_EXAMPLES "Build entityfy core examples" ON)
option(ENTITYFY_BUILD_DEMO "Build entityfy console, benchmark and migration probe" ON)
option(ENTITYFY_ENABLE_SQLITE "Build the SQLite backend" ON)
option(ENTITYFY_ENABLE_POSTGRESQL "Build the PostgreSQL backend" ON)

include(cmake/Dependencies.cmake)
include(CTest)

if(BUILD_TESTING AND ENTITYFY_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# Modules
# ---------------------------------------------------------------------------
add_subdirectory(modules/entityfy-core)
add_subdirectory(modules/entityfy-migrate)
add_subdirectory(modules/entityfy-context)

if(ENTITYFY_BUILD_DEMO)
    add_subdirectory(modules/entityfy-demo)
endif()
