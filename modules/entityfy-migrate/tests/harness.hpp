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

// Replays a provider dump in order, including SET/transaction control so
// identity inserts and quoted identifiers survive the round trip. Comment lines
// are dropped; the controlled test data makes splitting on ';' enough.
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

        const auto statement = dump.substr(start, end - start);
        start = end + 1;

        std::string cleaned;
        std::size_t line_start = 0;

        while(line_start <= statement.size())
        {
            const auto line_end = statement.find('\n', line_start);
            const auto line = statement.substr(
                line_start, line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);
            const auto first = line.find_first_not_of(" \t\r");

            if(first != std::string_view::npos && !line.substr(first).starts_with("--"))
            {
                cleaned.append(line);
                cleaned.push_back('\n');
            }

            if(line_end == std::string_view::npos)
            {
                break;
            }

            line_start = line_end + 1;
        }

        const auto first = cleaned.find_first_not_of(" \t\r\n");
        if(first == std::string::npos)
        {
            continue;
        }

        backend.execute_sql(std::string_view{cleaned}.substr(first));
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
