#pragma once

#include <filesystem>
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

namespace entityfy_test
{

enum class status
{
    passed,
    skipped,
    failed,
};

} // namespace entityfy_test
