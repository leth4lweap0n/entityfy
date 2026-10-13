#pragma once

#include <entityfy/migrate/diff.hpp>
#include <entityfy/migrate/provider.hpp>
#include <entityfy/migrate/schema.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace entityfy::migrate
{

struct migration_file
{
    std::string id;
    std::string created_at;
    std::string provider;
    schema      before;
    schema      after;
    std::string checksum;
};

struct engine_options
{
    std::filesystem::path root;            // migrations/<provider>
    std::string           provider_name;   // "sqlite" | "postgres"
    bool                  allow_destructive = false;
    bool                  backup = true;
    bool                  dry_run = false;
    std::map<std::string, std::string, std::less<>> renames;
};

struct apply_report
{
    std::vector<std::string> applied;
    std::vector<std::string> already_applied;
    std::vector<std::string> pending;
    bool                     dry_run = false;
    bool                     destructive = false;
};

class engine
{
public:
    engine(provider& backend, engine_options options);

    // migrations/<provider>/schema.snapshot.json
    void write_snapshot(const schema& current) const;
    [[nodiscard]] std::optional<schema> read_snapshot() const;

    // Writes the difference between the snapshot (an empty schema when absent) and current
    // into a migration file and updates the snapshot to current. Does not require a database.
    [[nodiscard]] migration_file add(const schema& current, std::string_view name) const;

    // migrations/<provider>/*.json, sorted by file name.
    [[nodiscard]] std::vector<migration_file> load_migrations() const;

    // Reads the applied ids from the database.
    [[nodiscard]] std::vector<std::string> applied_ids() const;

    [[nodiscard]] std::vector<migration_file> pending_migrations() const;

    // Applies pending migrations in order; each one is a single transaction.
    apply_report apply(const schema& current, std::ostream& out);

    // Produces only the SQL plan without applying it (dry-run).
    apply_report plan_only(const schema& current, std::ostream& out);

    void status(const schema& current, std::ostream& out);

    // Produces an executable SQL script for pending migrations.
    void script_pending(const schema& current, std::ostream& out);

    // Produces a SQL script containing every migration from scratch.
    void script_all(std::ostream& out);

    // Marks the existing database as "applied" without running the migrations.
    void baseline(const schema& current, bool force, std::ostream& out);

    // Reverts the last applied migration (returns to the before schema).
    void rollback_last(std::ostream& out);

private:
    [[nodiscard]] std::string history_table() const;
    void ensure_history() const;
    void write_script(const std::vector<migration_file>& migrations, int sequence, std::ostream& out) const;
    [[nodiscard]] std::string checksum_of(const migration_file& value) const;
    void apply_migration(const migration_file& value, bool inverse);

    provider*      backend_;
    engine_options options_;
    mutable bool   history_ready_ = false;
};

} // namespace entityfy::migrate
