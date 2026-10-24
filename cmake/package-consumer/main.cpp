#include <entityfy/db_context.hpp>
#include <entityfy/migrate/provider.hpp>

#include <iostream>

// Minimal installed-package smoke test: the headers and all three targets must
// resolve through find_package(entityfy), and SQLite must link and run.
int main()
{
    auto backend = entityfy::migrate::open_sqlite_provider(":memory:");
    const auto rows = backend->query("SELECT 42");
    if(rows.size() != 1 || rows.front().size() != 1 || rows.front().front() != "42")
    {
        return 1;
    }

    std::cout << "entityfy package consumer ok\n";
    return 0;
}
