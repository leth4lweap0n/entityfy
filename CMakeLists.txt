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
# MinGW runtime
# ---------------------------------------------------------------------------
# The MinGW toolchain normally ships libstdc++-6.dll, libgcc_s_seh-1.dll and
# libwinpthread-1.dll next to the compiler. Linking them statically keeps the
# produced executables free of those three DLLs so they run on machines that
# do not have MinGW installed. libpq stays a dynamic import, third-party SDK
# DLLs are not part of this block.
#
# Debug is excluded on purpose: GCC 16.2 + MinGW fails to link Debug binaries
# against the static libstdc++ (duplicate std::runtime_error definition from
# the inline header copy and cow-stdexcept.o, only pulled in when the code is
# not optimized). Release-like configurations keep the fully static runtime;
# Debug uses the MinGW DLLs, which is what a development build wants anyway.
if(MINGW)
    add_link_options(
        "$<$<NOT:$<CONFIG:Debug>>:-static-libgcc>"
        "$<$<NOT:$<CONFIG:Debug>>:-static-libstdc++>"
        "$<$<NOT:$<CONFIG:Debug>>:-static>"
    )
endif()

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
