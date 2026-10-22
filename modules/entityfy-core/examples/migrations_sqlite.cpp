#include <entityfy/entityfy.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <spdlog/spdlog.h>

struct [[= entityfy::table{"customers"}]] Customer
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::unique, = entityfy::indexed, = entityfy::not_null, = entityfy::varchar{320}]]
    std::string email;

    [[= entityfy::not_null, = entityfy::varchar{160}]]
    std::string display_name;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};

    [[= entityfy::updated_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point updated_at{};
};

struct [[= entityfy::table{"invoices"}]] Invoice
{
    [[= entityfy::id, = entityfy::auto_increment]]
    std::int64_t id = 0;

    [[= entityfy::indexed, = entityfy::not_null, = entityfy::references{"customers", "id", "CASCADE", ""}]]
    std::int64_t customer_id = 0;

    [[= entityfy::not_null, = entityfy::decimal{12, 2}]]
    double total = 0.0;

    [[= entityfy::not_null, = entityfy::varchar{40}, = entityfy::default_value{"'open'"}]]
    std::string status = "open";

    [[= entityfy::nullable, = entityfy::date]]
    std::optional<std::chrono::sys_days> due_on;

    [[= entityfy::created_at, = entityfy::timestamp]]
    std::chrono::system_clock::time_point created_at{};
};

void create_legacy_table_if_missing(entityfy::detail::backend& sqlite)
{
    if(sqlite.inspect_table("customers").exists())
    {
        return;
    }

    sqlite.execute(entityfy::statement{
        .sql =
            "CREATE TABLE \"customers\" ("
            "\"id\" INTEGER PRIMARY KEY AUTOINCREMENT, "
            "\"email\" TEXT, "
            "\"full_name\" TEXT"
            ")",
    });

    sqlite.execute(entityfy::statement{
        .sql = "INSERT INTO \"customers\" (\"email\", \"full_name\") VALUES (?, ?)",
        .binds = {std::string{"legacy@example.test"}, std::string{"Legacy Customer"}},
    });
}

int main()
{
    auto sqlite = entityfy::detail::make_sqlite_backend(":memory:");
    create_legacy_table_if_missing(*sqlite);

    entityfy::table_client<Customer> customers{*sqlite};
    entityfy::table_client<Invoice> invoices{*sqlite};

    const auto drift = customers.validate_schema();
    if(!drift.valid())
    {
        spdlog::warn("legacy customers table has {} schema issue(s)", drift.issues.size());
    }

    try
    {
        customers.migrate(entityfy::schema_sync_options{
            .validate_after = false,
        });
        customers.require_schema();
    }
    catch(const entityfy::orm_error& error)
    {
        spdlog::warn("additive migration could not fix destructive drift: {}", error.what());
    }

    customers.migrate_force();
    invoices.migrate();

    const auto customer = customers.insert(Customer{
        .email = "billing@example.test",
        .display_name = "Billing Team",
    });

    [[maybe_unused]] const auto invoice = invoices.insert(Invoice{
        .customer_id = customer.last_insert_id,
        .total = 199.95,
        .status = "open",
        .due_on = std::chrono::sys_days{std::chrono::year{2026} / 6 / 15},
    });

    entityfy::apply_migration(*sqlite, entityfy::migration{
        .id = "002_invoice_open_index",
        .statements = {
            entityfy::statement{
                .sql =
                    "CREATE INDEX IF NOT EXISTS \"idx_invoices_open\" "
                    "ON \"invoices\" (\"status\") WHERE \"status\" = 'open'",
            },
        },
    });

    spdlog::info("open invoices: {}", invoices.count(entityfy::where(&Invoice::status).eq("open")));
}
