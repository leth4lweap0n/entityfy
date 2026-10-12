#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace entityfy::migrate
{

[[nodiscard]] std::string utc_timestamp();                 // YYYYMMDDHHMMSS
[[nodiscard]] std::string slugify(std::string_view input); // lowercase, alnum and _

void ensure_directory(const std::filesystem::path& path);

} // namespace entityfy::migrate
