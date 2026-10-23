#include <entityfy/migrate/engine.hpp>

#include <entityfy/migrate/diff.hpp>
#include <entityfy/migrate/json.hpp>
#include <entityfy/migrate/sha256.hpp>
#include <entityfy/migrate/util.hpp>

#include <algorithm>
#include <filesystem>
#include <set>
#include <sstream>
#include <stdexcept>

namespace entityfy::migrate
{
namespace
{

constexpr std::string_view snapshot_filename = "schema.snapshot.json";

std::filesystem::path snapshot_path(const engine_options& options)
{
    return options.root / std::string{snapshot_filename};
}

std::vector<std::string> first_column(const std::vector<std::vector<std::string>>& rows)
{
    std::vector<std::string> output;
    output.reserve(rows.size());

    for(const auto& row: rows)
    {
        if(!row.empty())
        {
            output.push_back(row.front());
        }
    }

    return output;
}

std::string join_operations(const std::vector<operation>& operations)
{
    std::ostringstream stream;
    bool first = true;

    for(const auto& value: operations)
    {
        if(!first)
        {
            stream << "; ";
        }

        stream << describe_operation(value);
        first = false;
    }

    return stream.str();
}

bool missing_history_error(const std::exception& error)
{
    const std::string message = error.what();
    const bool names_history = message.find("entityfy_schema_migrations") != std::string::npos;

    return names_history && (message.find("no such table") != std::string::npos ||
                              message.find("does not exist") != std::string::npos);
}

diff_options comparison_options_for(const provider& backend, const engine_options& options)
{
    diff_options result = backend.comparison_options();
    result.renames = options.renames;
    return result;
}

} // namespace

engine::engine(provider& backend, engine_options options)
    : backend_{&backend}, options_{std::move(options)}
{
    if(options_.provider_name.empty())
    {
        options_.provider_name = backend_->name();
    }

    if(options_.root.empty())
    {
        options_.root = std::filesystem::path{"migrations"} / options_.provider_name;
    }
}

void engine::write_snapshot(const schema& current) const
{
    write_file(snapshot_path(options_), schema_to_json(current));
}

std::optional<schema> engine::read_snapshot() const
{
    const auto path = snapshot_path(options_);
    if(!std::filesystem::exists(path))
    {
        return std::nullopt;
    }

    return schema_from_json(read_file(path));
}

migration_file engine::add(const schema& current, std::string_view name) const
{
    const schema before = read_snapshot().value_or(schema{});

    const auto operations = diff_schemas(current, before, comparison_options_for(*backend_, options_));
    if(operations.empty())
    {
        throw std::runtime_error{"entityfy-migrate: no schema changes detected; nothing to migrate"};
    }

    const std::string timestamp = utc_timestamp();

    int sequence = 1;
    if(std::filesystem::exists(options_.root))
    {
        for(const auto& entry: std::filesystem::directory_iterator{options_.root})
        {
            if(entry.is_regular_file() && entry.path().extension() == ".json" &&
               entry.path().filename() != snapshot_filename)
            {
                ++sequence;
            }
        }
    }

    std::string padded_sequence = std::to_string(sequence);
    while(padded_sequence.size() < 3)
    {
        padded_sequence.insert(padded_sequence.begin(), '0');
    }

    migration_file file;
    file.id = timestamp + "_" + padded_sequence + "_" + slugify(name);
    file.created_at = timestamp;
    file.provider = options_.provider_name;
    file.before = before;
    file.after = current;
    file.checksum = checksum_of(file);

    write_file(options_.root / (file.id + ".json"), migration_to_json(file));
    write_snapshot(current);

    return file;
}

std::vector<migration_file> engine::load_migrations() const
{
    std::vector<std::filesystem::path> paths;

    if(std::filesystem::exists(options_.root))
    {
        for(const auto& entry: std::filesystem::directory_iterator{options_.root})
        {
            if(!entry.is_regular_file() || entry.path().extension() != ".json")
            {
                continue;
            }

            if(entry.path().filename() == snapshot_filename)
            {
                continue;
            }

            paths.push_back(entry.path());
        }
    }

    std::ranges::sort(paths);

    std::vector<migration_file> output;
    output.reserve(paths.size());

    for(const auto& path: paths)
    {
        auto file = migration_from_json(read_file(path));

        if(!file.provider.empty() && file.provider != options_.provider_name)
        {
            throw std::runtime_error{"entityfy-migrate: migration " + path.string() + " belongs to provider " + file.provider};
        }

        output.emplace_back(std::move(file));
    }

    return output;
}

std::string engine::history_table() const
{
    return "entityfy_schema_migrations";
}

void engine::ensure_history() const
{
    if(history_ready_)
    {
        return;
    }

    const std::string history = history_table();

    const bool        mysql = backend_->name() == "mysql";
    const bool        sqlserver = backend_->name() == "sqlserver";
    const std::string id_type = sqlserver ? "NVARCHAR(191)" : (mysql ? "VARCHAR(191)" : "TEXT");
    const std::string checksum_type = sqlserver ? "NVARCHAR(64)" : (mysql ? "VARCHAR(64)" : "TEXT");
    const std::string applied_type = sqlserver ? "DATETIME2" : (mysql ? "DATETIME" : "TEXT");

    if(sqlserver)
    {
        backend_->execute_sql(
            "IF OBJECT_ID(N'" + history + "', N'U') IS NULL CREATE TABLE \"" + history + "\" ("
            "\"id\" " + id_type + " PRIMARY KEY, "
            "\"checksum\" " + checksum_type + " NOT NULL DEFAULT '', "
            "\"seq\" INTEGER NOT NULL DEFAULT 0, "
            "\"applied_at\" " + applied_type + " NOT NULL DEFAULT CURRENT_TIMESTAMP)");
    }
    else
    {
        backend_->execute_sql(
            "CREATE TABLE IF NOT EXISTS \"" + history + "\" ("
            "\"id\" " + id_type + " PRIMARY KEY, "
            "\"checksum\" " + checksum_type + " NOT NULL DEFAULT '', "
            "\"seq\" INTEGER NOT NULL DEFAULT 0, "
            "\"applied_at\" " + applied_type + " NOT NULL DEFAULT CURRENT_TIMESTAMP"
            ")");
    }

    bool has_sequence = true;
    try
    {
        if(sqlserver)
        {
            (void)backend_->query("SELECT TOP (1) \"seq\" FROM \"" + history + "\"");
        }
        else
        {
            (void)backend_->query("SELECT \"seq\" FROM \"" + history + "\" LIMIT 1");
        }
    }
    catch(...)
    {
        has_sequence = false;
    }

    if(!has_sequence)
    {
        backend_->execute_sql(
            "ALTER TABLE \"" + history + "\" ADD COLUMN \"seq\" INTEGER NOT NULL DEFAULT 0");
    }

    history_ready_ = true;
}

std::string engine::checksum_of(const migration_file& value) const
{
    const std::string payload = schema_to_json(value.before) + "\n" + schema_to_json(value.after) + "\n" +
                                value.provider;
    return sha256_hex(payload);
}

std::vector<std::string> engine::applied_ids() const
{
    ensure_history();

    return first_column(backend_->query(
        "SELECT \"id\" FROM \"entityfy_schema_migrations\" ORDER BY \"seq\", \"id\""));
}

std::vector<migration_file> engine::pending_migrations() const
{
    const auto applied = applied_ids();
    const std::set<std::string> applied_set(applied.begin(), applied.end());

    std::vector<migration_file> output;
    for(auto& file: load_migrations())
    {
        if(!applied_set.contains(file.id))
        {
            output.emplace_back(std::move(file));
        }
    }

    return output;
}

apply_report engine::plan_only(const schema& current, std::ostream& out)
{
    const bool saved = options_.dry_run;
    options_.dry_run = true;

    try
    {
        auto report = apply(current, out);
        options_.dry_run = saved;
        return report;
    }
    catch(...)
    {
        options_.dry_run = saved;
        throw;
    }
}

apply_report engine::apply(const schema& current, std::ostream& out)
{
    ensure_history();

    const auto applied = applied_ids();
    const std::set<std::string> applied_set(applied.begin(), applied.end());

    apply_report report;
    std::vector<migration_file> to_apply;

    for(auto& file: load_migrations())
    {
        if(applied_set.contains(file.id))
        {
            report.already_applied.push_back(file.id);
        }
        else
        {
            report.pending.push_back(file.id);
            to_apply.emplace_back(std::move(file));
        }
    }

    if(to_apply.empty())
    {
        out << "entityfy-migrate: no pending migrations\n";
        return report;
    }

    for(const auto& file: to_apply)
    {
        const auto expected_checksum = checksum_of(file);
        if(!file.checksum.empty() && file.checksum != expected_checksum)
        {
            throw std::runtime_error{"entityfy-migrate: migration " + file.id + " checksum mismatch (file was modified)"};
        }

        const schema actual = backend_->introspect();
        const auto drift = diff_schemas(file.before, actual, comparison_options_for(*backend_, options_));
        if(!drift.empty())
        {
            throw std::runtime_error{"entityfy-migrate: database does not match the expected state before " + file.id + ": " +
                                     join_operations(drift)};
        }

        const plan migration_plan = backend_->make_plan(file.after, file.before, comparison_options_for(*backend_, options_));
        report.destructive = report.destructive || migration_plan.destructive;

        out << "== " << file.id << "\n";
        for(const auto& step: migration_plan.steps)
        {
            out << "  " << (step.type == step::kind::sqlite_rebuild ? "[rebuild] " : "") << step.sql << "\n";
        }

        for(const auto& warning: migration_plan.warnings)
        {
            out << "  warning: " << warning << "\n";
        }

        if(migration_plan.destructive && !options_.allow_destructive)
        {
            throw std::runtime_error{"entityfy-migrate: " + file.id +
                                     " contains destructive operations; re-run with --allow-destructive"};
        }

        if(options_.dry_run)
        {
            report.dry_run = true;
            continue;
        }

        if(options_.backup && migration_plan.destructive)
        {
            const auto backup_path = options_.root / ("backup-" + file.id + ".db");
            backend_->backup(backup_path.string());
            out << "  backup: " << backup_path.string() << "\n";
        }

        try
        {
            backend_->start();

            for(const auto& step: migration_plan.steps)
            {
                backend_->execute(step);
            }

            const auto sequence_rows = backend_->query(
                "SELECT COALESCE(MAX(\"seq\"), 0) + 1 FROM \"entityfy_schema_migrations\"");
            const std::string sequence = sequence_rows.empty() || sequence_rows.front().empty()
                                             ? std::string{"1"}
                                             : sequence_rows.front().front();

            backend_->execute_params(
                "INSERT INTO \"entityfy_schema_migrations\" (\"id\", \"checksum\", \"seq\") VALUES (?, ?, ?)",
                {file.id, expected_checksum, sequence});

            const schema after = backend_->introspect();
            const auto remaining = diff_schemas(file.after, after, backend_->comparison_options());
            if(!remaining.empty())
            {
                throw std::runtime_error{"entityfy-migrate: post-apply validation failed for " + file.id + ": " +
                                         join_operations(remaining)};
            }

            backend_->finish();
        }
        catch(...)
        {
            backend_->abort();
            throw;
        }

        report.applied.push_back(file.id);
    }

    if(!options_.dry_run)
    {
        const schema actual = backend_->introspect();
        const auto drift = diff_schemas(current, actual, comparison_options_for(*backend_, options_));
        if(!drift.empty())
        {
            throw std::runtime_error{"entityfy-migrate: database still drifts from the model after apply: " +
                                     join_operations(drift)};
        }
    }

    return report;
}

void engine::script_pending(const schema& current, std::ostream& out)
{
    std::set<std::string> applied_set;
    try
    {
        for(const auto& id: first_column(backend_->query(
                "SELECT \"id\" FROM \"entityfy_schema_migrations\" ORDER BY \"seq\", \"id\"")))
        {
            applied_set.insert(id);
        }
    }
    catch(const std::exception& error)
    {
        if(!missing_history_error(error))
        {
            throw;
        }
    }

    std::vector<migration_file> pending;
    for(const auto& file: load_migrations())
    {
        if(!applied_set.contains(file.id))
        {
            pending.emplace_back(file);
        }
    }

    if(pending.empty())
    {
        out << "-- no pending migrations\n";
        return;
    }

    const schema actual = backend_->introspect();
    const auto drift = diff_schemas(pending.front().before, actual, comparison_options_for(*backend_, options_));
    if(!drift.empty())
    {
        throw std::runtime_error{"entityfy-migrate: database does not match the expected state before " + pending.front().id +
                                 ": " + join_operations(drift)};
    }

    int sequence = 0;
    try
    {
        const auto sequence_rows = backend_->query(
            "SELECT COALESCE(MAX(\"seq\"), 0) FROM \"entityfy_schema_migrations\"");
        sequence = sequence_rows.empty() || sequence_rows.front().empty()
                       ? 0
                       : std::stoi(sequence_rows.front().front());
    }
    catch(const std::exception& error)
    {
        if(!missing_history_error(error))
        {
            throw;
        }

        sequence = 0;
    }

    write_script(pending, sequence, out);
}

void engine::write_script(const std::vector<migration_file>& migrations, int sequence, std::ostream& out) const
{
    if(migrations.empty())
    {
        out << "-- no migrations\n";
        return;
    }

    out << "-- entityfy-migrate script (" << options_.provider_name << ")\n";
    out << "CREATE TABLE IF NOT EXISTS \"entityfy_schema_migrations\" ("
        << "\"id\" " << (options_.provider_name == "mysql" ? "VARCHAR(191)" : "TEXT") << " PRIMARY KEY, "
        << "\"checksum\" TEXT NOT NULL DEFAULT '', "
        << "\"seq\" INTEGER NOT NULL DEFAULT 0, "
        << "\"applied_at\" " << (options_.provider_name == "mysql" ? "DATETIME" : "TEXT")
        << " NOT NULL DEFAULT CURRENT_TIMESTAMP);\n";

    const bool sqlite = options_.provider_name == "sqlite";
    if(sqlite)
    {
        out << ".bail on\n";
        out << "PRAGMA foreign_keys = OFF;\n";
    }
    else
    {
        out << "\\set ON_ERROR_STOP on\n";
    }

    for(const auto& file: migrations)
    {
        ++sequence;

        out << "\n-- migration " << file.id << "\nBEGIN;\n";

        const plan migration_plan =
            backend_->make_plan(file.after, file.before, comparison_options_for(*backend_, options_));

        for(const auto& step: migration_plan.steps)
        {
            if(step.type == step::kind::sql)
            {
                out << step.sql << ";\n";
                continue;
            }

            for(const auto& sql: backend_->render_rebuild(step.desired_table, step.actual_table))
            {
                out << sql << ";\n";
            }
        }

        out << "INSERT INTO \"entityfy_schema_migrations\" (\"id\", \"checksum\", \"seq\") VALUES ("
            << quote_literal(file.id) << ", " << quote_literal(checksum_of(file)) << ", " << sequence << ");\n";
        out << "COMMIT;\n";
    }

    if(sqlite)
    {
        out << "\nPRAGMA foreign_key_check;\nPRAGMA foreign_keys = ON;\n";
    }
}

void engine::script_all(std::ostream& out)
{
    write_script(load_migrations(), 0, out);
}

void engine::status(const schema& current, std::ostream& out)
{
    ensure_history();

    const auto applied = applied_ids();
    const std::set<std::string> applied_set(applied.begin(), applied.end());

    out << "applied migrations: " << applied.size() << "\n";
    for(const auto& id: applied)
    {
        out << "  [x] " << id << "\n";
    }

    for(const auto& file: load_migrations())
    {
        if(!applied_set.contains(file.id))
        {
            out << "  [ ] " << file.id << " (pending)\n";
        }
    }

    const schema actual = backend_->introspect();
    const auto drift = diff_schemas(current, actual, comparison_options_for(*backend_, options_));
    if(drift.empty())
    {
        out << "schema: in sync\n";
    }
    else
    {
        out << "schema: DRIFT (" << drift.size() << ")\n";
        for(const auto& value: drift)
        {
            out << "  - " << describe_operation(value) << "\n";
        }
    }
}

void engine::baseline(const schema& current, bool force, std::ostream& out)
{
    ensure_history();

    const schema actual = backend_->introspect();
    const auto drift = diff_schemas(current, actual, comparison_options_for(*backend_, options_));
    if(!drift.empty() && !force)
    {
        throw std::runtime_error{"entityfy-migrate: database does not match the models; re-run with --force to baseline anyway: " +
                                 join_operations(drift)};
    }

    const auto applied = applied_ids();
    const std::set<std::string> applied_set(applied.begin(), applied.end());

    try
    {
        backend_->start();
        const auto sequence_rows = backend_->query(
            "SELECT COALESCE(MAX(\"seq\"), 0) FROM \"entityfy_schema_migrations\"");
        int sequence = sequence_rows.empty() || sequence_rows.front().empty()
                           ? 0
                           : std::stoi(sequence_rows.front().front());

        for(const auto& file: load_migrations())
        {
            if(applied_set.contains(file.id))
            {
                continue;
            }

            ++sequence;
            backend_->execute_params(
                "INSERT INTO \"entityfy_schema_migrations\" (\"id\", \"checksum\", \"seq\") VALUES (?, ?, ?)",
                {file.id, checksum_of(file), std::to_string(sequence)});
            out << "baseline: " << file.id << "\n";
        }

        backend_->finish();
    }
    catch(...)
    {
        backend_->abort();
        throw;
    }
}

void engine::rollback_last(std::ostream& out)
{
    ensure_history();

    const auto applied = applied_ids();
    if(applied.empty())
    {
        throw std::runtime_error{"entityfy-migrate: no applied migrations to roll back"};
    }

    const std::string target = applied.back();

    const auto migrations = load_migrations();
    const auto found = std::ranges::find_if(migrations, [&](const migration_file& value) {
        return value.id == target;
    });

    if(found == migrations.end())
    {
        throw std::runtime_error{"entityfy-migrate: migration file for " + target + " was not found"};
    }

    diff_options inverse_options = backend_->comparison_options();
    for(const auto& [key, target_name]: options_.renames)
    {
        const auto separator = key.find('.');
        if(separator == std::string::npos)
        {
            continue;
        }

        inverse_options.renames[key.substr(0, separator) + "." + target_name] = key.substr(separator + 1);
    }

    const plan migration_plan = backend_->make_plan(found->before, found->after, inverse_options);
    out << "== rollback " << target << "\n";
    for(const auto& step: migration_plan.steps)
    {
        out << "  " << (step.type == step::kind::sqlite_rebuild ? "[rebuild] " : "") << step.sql << "\n";
    }

    if(migration_plan.destructive && !options_.allow_destructive)
    {
        throw std::runtime_error{"entityfy-migrate: rollback of " + target +
                                 " is destructive; re-run with --allow-destructive"};
    }

    if(options_.backup && migration_plan.destructive)
    {
        const auto backup_path = options_.root / ("backup-rollback-" + target + ".db");
        backend_->backup(backup_path.string());
        out << "  backup: " << backup_path.string() << "\n";
    }

    try
    {
        backend_->start();
        for(const auto& step: migration_plan.steps)
        {
            backend_->execute(step);
        }

        backend_->execute_params("DELETE FROM \"entityfy_schema_migrations\" WHERE \"id\" = ?", {target});

        const schema after = backend_->introspect();
        const auto remaining = diff_schemas(found->before, after, inverse_options);
        if(!remaining.empty())
        {
            throw std::runtime_error{"entityfy-migrate: rollback validation failed: " + join_operations(remaining)};
        }

        backend_->finish();
    }
    catch(...)
    {
        backend_->abort();
        throw;
    }

    out << "entityfy-migrate: rolled back " << target << "\n";
}

} // namespace entityfy::migrate
