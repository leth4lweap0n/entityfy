#pragma once

#include <entityfy/migrate/provider.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

// Test files are written to a temporary working area, not into the source tree.
inline std::filesystem::path test_workspace(std::string_view name)
{
    auto root = std::filesystem::temp_directory_path() / "entityfy_tests";
    std::filesystem::create_directories(root);
    return root / std::filesystem::path{std::string{name}};
}

inline void require(bool condition, std::string_view message)
{
    if(!condition)
    {
        throw std::runtime_error{std::string{message}};
    }
}

inline std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// Replays the CREATE/INSERT statements of a provider dump (controlled test data,
// so splitting on ';' is enough).
inline void restore_dump(entityfy::migrate::provider& backend, std::string_view dump)
{
    std::size_t start = 0;

    while(start < dump.size())
    {
        const auto end = dump.find(';', start);
        if(end == std::string_view::npos)
        {
            break;
        }

        auto statement = dump.substr(start, end - start);
        start = end + 1;

        const auto first = statement.find_first_not_of(" \t\r\n");
        if(first == std::string_view::npos)
        {
            continue;
        }

        statement.remove_prefix(first);

        if(statement.starts_with("CREATE") || statement.starts_with("INSERT"))
        {
            backend.execute_sql(statement);
        }
    }
}

namespace entityfy_test
{

enum class status
{
    passed,
    skipped,
    failed,
};

} // namespace entityfy_test
