# Third party dependencies for the entityfy monorepo.
#
# Everything is fetched once at the top level so that all modules share the
# same SQLite build (core, migrations and the benchmark all link `sl3`).
# This removes the duplicated vendored SQLite copy that used to live inside
# the migration module.

include(FetchContent)

# --- SQLite (via sl3, embeds sqlite3) ---------------------------------------
if(ENTITYFY_ENABLE_SQLITE)
    FetchContent_Declare(
        sl3
        GIT_REPOSITORY https://github.com/a4z/libsl3.git
        GIT_TAG v1.2.53003
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
    )
    set(sl3_USE_INTERNAL_SQLITE3 ON CACHE BOOL "Use bundled SQLite" FORCE)
    set(sl3_USE_COMMON_COMPILER_WARNINGS OFF CACHE BOOL "Use common compiler warning preset target" FORCE)
    FetchContent_MakeAvailable(sl3)

    # Shared access point for the embedded SQLite: headers and symbols.
    add_library(entityfy_sqlite INTERFACE)
    target_link_libraries(entityfy_sqlite INTERFACE sl3)
    add_library(entityfy::sqlite ALIAS entityfy_sqlite)
endif()

# --- PostgreSQL (libpq, used by the core backend) --------------------------
if(ENTITYFY_ENABLE_POSTGRESQL)
    find_package(PostgreSQL QUIET)

    if(NOT PostgreSQL_FOUND)
        message(STATUS
            "PostgreSQL client library not found: the PostgreSQL backend stays disabled. "
            "Set PostgreSQL_ROOT, or PostgreSQL_INCLUDE_DIR and PostgreSQL_LIBRARY, to enable it.")
        set(ENTITYFY_ENABLE_POSTGRESQL OFF CACHE BOOL "Build the PostgreSQL backend" FORCE)
    endif()
endif()

# --- MySQL / MariaDB (Connector/C, used by the core backend) --------------
if(ENTITYFY_ENABLE_MYSQL)
    find_path(MYSQL_INCLUDE_DIR NAMES mysql.h HINTS ${MYSQL_ROOT} PATH_SUFFIXES include include/mariadb)
    find_library(MYSQL_LIBRARY NAMES mariadb libmariadb mysqlclient HINTS ${MYSQL_ROOT} PATH_SUFFIXES lib lib/mariadb)

    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        add_library(entityfy_mysql UNKNOWN IMPORTED)
        set_target_properties(entityfy_mysql PROPERTIES
            IMPORTED_LOCATION "${MYSQL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}"
        )
        add_library(entityfy::mysql ALIAS entityfy_mysql)
    else()
        message(STATUS
            "MariaDB Connector/C not found: the MySQL backend stays disabled. "
            "Set MYSQL_ROOT, or MYSQL_INCLUDE_DIR and MYSQL_LIBRARY, to enable it.")
        set(ENTITYFY_ENABLE_MYSQL OFF CACHE BOOL "Build the MySQL/MariaDB backend" FORCE)
    endif()
endif()

# --- SQL Server (ODBC, used by the core backend) --------------------------
if(ENTITYFY_ENABLE_SQLSERVER)
    find_package(ODBC QUIET)

    if(NOT ODBC_FOUND)
        message(STATUS
            "ODBC not found: the SQL Server backend stays disabled. Install unixODBC (Linux) or use the "
            "Windows ODBC driver manager, and set ODBC_ROOT if needed.")
        set(ENTITYFY_ENABLE_SQLSERVER OFF CACHE BOOL "Build the SQL Server backend" FORCE)
    endif()
endif()

# --- JSON (migration snapshots and scaffold) -------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Test framework --------------------------------------------------------
if(ENTITYFY_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.14.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# --- Logging for the core examples ----------------------------------------
if(ENTITYFY_BUILD_EXAMPLES)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.x
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(spdlog)
endif()
