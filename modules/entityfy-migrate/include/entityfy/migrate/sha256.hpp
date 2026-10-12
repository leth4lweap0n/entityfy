#pragma once

#include <string>
#include <string_view>

namespace entityfy::migrate
{

[[nodiscard]] std::string sha256_hex(std::string_view input);

} // namespace entityfy::migrate
