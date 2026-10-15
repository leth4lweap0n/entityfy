#include <entityfy/migrate/provider.hpp>
#include <entityfy/migrate/schema.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace entityfy::migrate
{

void rename_table(provider& backend, std::string_view from, std::string_view to)
{
    if(from.empty() || to.empty())
    {
        throw std::invalid_argument{"entityfy-migrate: table names cannot be empty"};
    }

    backend.execute_sql("ALTER TABLE " + quote_ident(from) + " RENAME TO " + quote_ident(to));
}

} // namespace entityfy::migrate
