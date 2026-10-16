#include "pq_dynamic.hpp"

#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace entityfy::migrate::pq
{
namespace
{

using pg_conn = void;
using pg_result = void;

using connectdb_fn = pg_conn* (*)(const char*);
using status_fn = int (*)(const pg_conn*);
using error_fn = char* (*)(const pg_conn*);
using finish_fn = void (*)(pg_conn*);
using exec_fn = pg_result* (*)(pg_conn*, const char*);
using exec_params_fn = pg_result* (*)(pg_conn*, const char*, int, const unsigned int*, const char* const*,
                                      const int*, const int*, int);
using result_status_fn = int (*)(const pg_result*);
using result_error_fn = char* (*)(const pg_result*);
using clear_fn = void (*)(pg_result*);
using ntuples_fn = int (*)(const pg_result*);
using nfields_fn = int (*)(const pg_result*);
using getvalue_fn = char* (*)(const pg_result*, int, int);
using getisnull_fn = int (*)(const pg_result*, int, int);
using cmd_tuples_fn = char* (*)(const pg_result*);

constexpr int connection_ok = 0;
constexpr int result_empty_query = 0;
constexpr int result_command_ok = 1;
constexpr int result_tuples_ok = 2;

struct api
{
    void* handle = nullptr;
    connectdb_fn connectdb = nullptr;
    status_fn status = nullptr;
    error_fn error_message = nullptr;
    finish_fn finish = nullptr;
    exec_fn exec = nullptr;
    exec_params_fn exec_params = nullptr;
    result_status_fn result_status = nullptr;
    result_error_fn result_error_message = nullptr;
    clear_fn clear = nullptr;
    ntuples_fn ntuples = nullptr;
    nfields_fn nfields = nullptr;
    getvalue_fn getvalue = nullptr;
    getisnull_fn getisnull = nullptr;
    cmd_tuples_fn cmd_tuples = nullptr;
    std::string extra_error;
};

void* open_library(const char* name)
{
#if defined(_WIN32)
    return static_cast<void*>(LoadLibraryA(name));
#else
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* handle, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

std::string platform_error()
{
#if defined(_WIN32)
    return "GetLastError=" + std::to_string(GetLastError());
#else
    const char* message = dlerror();
    return message == nullptr ? "unknown dlopen error" : message;
#endif
}

api load_api()
{
    api output;

    std::vector<std::string> candidates;
    if(const char* explicit_path = std::getenv("RMG_LIBPQ"))
    {
        candidates.emplace_back(explicit_path);
    }

#if defined(_WIN32)
    candidates.emplace_back("libpq.dll");
    if(const char* bin_dir = std::getenv("RMG_PG_BIN"))
    {
        candidates.emplace_back(std::string{bin_dir} + "\\libpq.dll");
    }
#else
    candidates.emplace_back("libpq.so.5");
    candidates.emplace_back("libpq.so");
#endif

    for(const auto& candidate: candidates)
    {
        output.handle = open_library(candidate.c_str());
        if(output.handle != nullptr)
        {
            break;
        }
    }

    if(output.handle == nullptr)
    {
        output.extra_error = "libpq not found (RMG_LIBPQ or PATH): " + platform_error();
        return output;
    }

    const auto bind = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(find_symbol(output.handle, name));
        if(function == nullptr)
        {
            output.extra_error = std::string{"libpq symbol not found: "} + name;
        }
    };

    bind(output.connectdb, "PQconnectdb");
    bind(output.status, "PQstatus");
    bind(output.error_message, "PQerrorMessage");
    bind(output.finish, "PQfinish");
    bind(output.exec, "PQexec");
    bind(output.exec_params, "PQexecParams");
    bind(output.result_status, "PQresultStatus");
    bind(output.result_error_message, "PQresultErrorMessage");
    bind(output.clear, "PQclear");
    bind(output.ntuples, "PQntuples");
    bind(output.nfields, "PQnfields");
    bind(output.getvalue, "PQgetvalue");
    bind(output.getisnull, "PQgetisnull");
    bind(output.cmd_tuples, "PQcmdTuples");

    return output;
}

api& get_api()
{
    static api instance = load_api();
    return instance;
}

std::string connection_error(const api& value, pg_conn* connection)
{
    if(connection == nullptr || value.error_message == nullptr)
    {
        return value.extra_error;
    }

    const char* message = value.error_message(connection);
    return message == nullptr ? std::string{"unknown libpq error"} : std::string{message};
}

std::string translate_placeholders(std::string_view sql)
{
    std::string output;
    output.reserve(sql.size() + 8);

    int index = 0;
    bool quoted = false;

    for(const char character: sql)
    {
        if(character == '\'')
        {
            quoted = !quoted;
            output.push_back(character);
            continue;
        }

        if(!quoted && character == '?')
        {
            ++index;
            output.push_back('$');
            output += std::to_string(index);
            continue;
        }

        output.push_back(character);
    }

    return output;
}

std::string result_error(const api& value, pg_result* result)
{
    if(result == nullptr || value.result_error_message == nullptr)
    {
        return "no libpq result";
    }

    const char* message = value.result_error_message(result);
    return message == nullptr ? std::string{"unknown libpq error"} : std::string{message};
}

std::vector<std::vector<std::string>> collect(const api& value, pg_result* result)
{
    std::vector<std::vector<std::string>> rows;

    const int row_count = value.ntuples(result);
    const int column_count = value.nfields(result);
    rows.reserve(static_cast<std::size_t>(row_count));

    for(int row = 0; row < row_count; ++row)
    {
        std::vector<std::string> current;
        current.reserve(static_cast<std::size_t>(column_count));

        for(int column = 0; column < column_count; ++column)
        {
            if(value.getisnull(result, row, column) != 0)
            {
                current.emplace_back();
            }
            else
            {
                const char* text = value.getvalue(result, row, column);
                current.emplace_back(text == nullptr ? "" : text);
            }
        }

        rows.emplace_back(std::move(current));
    }

    return rows;
}

bool is_success(int status)
{
    return status == result_empty_query || status == result_command_ok || status == result_tuples_ok;
}

} // namespace

bool available()
{
    return get_api().handle != nullptr;
}

std::string last_error()
{
    return get_api().extra_error;
}

database::database(std::string connection_string)
{
    api& value = get_api();
    if(value.handle == nullptr)
    {
        throw std::runtime_error{"entityfy/migrate:postgres: " + value.extra_error};
    }

    connection_ = value.connectdb(connection_string.c_str());
    if(connection_ == nullptr)
    {
        throw std::runtime_error{"entityfy/migrate:postgres: PQconnectdb failed"};
    }

    if(value.status(static_cast<pg_conn*>(connection_)) != connection_ok)
    {
        const std::string message = connection_error(value, static_cast<pg_conn*>(connection_));
        value.finish(static_cast<pg_conn*>(connection_));
        connection_ = nullptr;
        throw std::runtime_error{"entityfy/migrate:postgres: connection could not be established: " + message};
    }

    exec("SET client_min_messages TO WARNING");
}

database::~database()
{
    if(connection_ != nullptr)
    {
        api& value = get_api();
        value.finish(static_cast<pg_conn*>(connection_));
        connection_ = nullptr;
    }
}

void database::exec(std::string_view sql)
{
    api& value = get_api();
    pg_result* result = value.exec(static_cast<pg_conn*>(connection_), std::string{sql}.c_str());
    if(result == nullptr)
    {
        throw std::runtime_error{"entityfy/migrate:postgres: " + connection_error(value, static_cast<pg_conn*>(connection_))};
    }

    const int status = value.result_status(result);
    if(!is_success(status))
    {
        const std::string message = result_error(value, result);
        value.clear(result);
        throw std::runtime_error{"entityfy/migrate:postgres: statement failed: " + message + " | SQL: " + std::string{sql}};
    }

    value.clear(result);
}

std::vector<std::vector<std::string>> database::query(std::string_view sql)
{
    api& value = get_api();
    pg_result* result = value.exec(static_cast<pg_conn*>(connection_), std::string{sql}.c_str());
    if(result == nullptr)
    {
        throw std::runtime_error{"entityfy/migrate:postgres: " + connection_error(value, static_cast<pg_conn*>(connection_))};
    }

    const int status = value.result_status(result);
    if(status == result_command_ok)
    {
        value.clear(result);
        return {};
    }

    if(status != result_tuples_ok)
    {
        const std::string message = result_error(value, result);
        value.clear(result);
        throw std::runtime_error{"entityfy/migrate:postgres: query failed: " + message + " | SQL: " + std::string{sql}};
    }

    auto rows = collect(value, result);
    value.clear(result);
    return rows;
}

std::vector<std::vector<std::string>> database::query_params(
    std::string_view sql,
    const std::vector<std::string>& params)
{
    api& value = get_api();

    const std::string translated = translate_placeholders(sql);

    std::vector<const char*> values;
    values.reserve(params.size());
    for(const auto& parameter: params)
    {
        values.push_back(parameter.c_str());
    }

    pg_result* result = value.exec_params(
        static_cast<pg_conn*>(connection_),
        translated.c_str(),
        static_cast<int>(params.size()),
        nullptr,
        values.data(),
        nullptr,
        nullptr,
        0);

    if(result == nullptr)
    {
        throw std::runtime_error{"entityfy/migrate:postgres: " + connection_error(value, static_cast<pg_conn*>(connection_))};
    }

    const int status = value.result_status(result);
    if(status == result_command_ok)
    {
        value.clear(result);
        return {};
    }

    if(status != result_tuples_ok)
    {
        const std::string message = result_error(value, result);
        value.clear(result);
        throw std::runtime_error{"entityfy/migrate:postgres: query failed: " + message + " | SQL: " + std::string{sql}};
    }

    auto rows = collect(value, result);
    value.clear(result);
    return rows;
}

std::string database::error() const
{
    return connection_error(get_api(), static_cast<pg_conn*>(connection_));
}

} // namespace entityfy::migrate::pq
