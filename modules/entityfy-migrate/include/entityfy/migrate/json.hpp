#pragma once

#include <entityfy/migrate/engine.hpp>
#include <entityfy/migrate/schema.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace entityfy::migrate
{

[[nodiscard]] std::string schema_to_json(const schema& value);
[[nodiscard]] schema      schema_from_json(std::string_view text);

[[nodiscard]] std::string      migration_to_json(const migration_file& value);
[[nodiscard]] migration_file   migration_from_json(std::string_view text);

void        write_file(const std::filesystem::path& path, std::string_view content);
[[nodiscard]] std::string read_file(const std::filesystem::path& path);

} // namespace entityfy::migrate
