#pragma once

#include <entityfy/migrate/schema.hpp>

#include <string>
#include <string_view>

namespace entityfy::migrate
{

// Generates an annotated C++ model header from the live schema.
[[nodiscard]] std::string generate_models(
    const schema& value,
    std::string_view name_space = "scaffold_models");

} // namespace entityfy::migrate
