#include <entityfy/migrate/util.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <stdexcept>

namespace entityfy::migrate
{

std::string utc_timestamp()
{
    const std::time_t now = std::time(nullptr);

    std::tm broken_down{};
#if defined(_WIN32)
    gmtime_s(&broken_down, &now);
#else
    gmtime_r(&now, &broken_down);
#endif

    char buffer[32]{};
    if(std::strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &broken_down) == 0)
    {
        throw std::runtime_error{"entityfy-migrate: utc_timestamp failed"};
    }

    return std::string{buffer};
}

std::string slugify(std::string_view input)
{
    std::string output;
    output.reserve(input.size());

    for(const char character: input)
    {
        if(std::isalnum(static_cast<unsigned char>(character)))
        {
            output.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
        else if(!output.empty() && output.back() != '_')
        {
            output.push_back('_');
        }
    }

    while(!output.empty() && output.back() == '_')
    {
        output.pop_back();
    }

    if(output.empty())
    {
        output = "migration";
    }

    return output;
}

void ensure_directory(const std::filesystem::path& path)
{
    if(path.empty())
    {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(path, error);
    if(error && !std::filesystem::is_directory(path))
    {
        throw std::runtime_error{"entityfy-migrate: cannot create directory " + path.string() + ": " + error.message()};
    }
}

} // namespace entityfy::migrate
