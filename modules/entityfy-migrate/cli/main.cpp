#include <entityfy/migrate/engine.hpp>
#include <entityfy/migrate/provider.hpp>
#include <entityfy/migrate/scaffold.hpp>
#include <entityfy/migrate/util.hpp>

#include "models.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace
{

struct arguments
{
    std::string command;
    std::string database;
    std::string provider;
    std::filesystem::path root;
    std::string name;
    bool allow_destructive = false;
    bool backup = true;
    bool dry_run = false;
    bool root_explicit = false;
    bool force = false;
    bool all = false;
    std::string output_path;
    std::string namespace_name = "scaffold_models";
    std::string rename_to;
    std::map<std::string, std::string, std::less<>> renames;
};

void print_help()
{
    std::cout <<
        "usage: entityfy_migrate <command> [options]\n"
        "\n"
        "commands:\n"
        "  snapshot                 writes schema.snapshot.json from the current models\n"
        "  add <name>               writes the difference between the snapshot and the models to a migration file\n"
        "  plan                     prints the SQL of pending migrations (does not apply them)\n"
        "  apply                    applies pending migrations inside a transaction\n"
        "  status                   applied/pending migrations and schema drift\n"
        "  script                   produces executable SQL for pending migrations\n"
        "  scaffold                 generates a C++ model header from the live schema\n"
        "  baseline                 marks the existing database without applying the migrations (--force)\n"
        "  rollback                 reverts the last applied migration\n"
        "\n"
        "options:\n"
        "  --db <uri>               sqlite://file.db or postgres://conninfo\n"
        "  --provider <name>        sqlite | postgres | mysql (for snapshot/add, defaults to sqlite)\n"
        "  --root <dir>             migration directory (defaults to migrations/<provider>)\n"
        "  --rename <old>=<new>     column rename hint (table.column=new_name by default)\n"
        "  --allow-destructive      allows destructive operations such as drop/type changes\n"
        "  --no-backup              disables taking a backup before a destructive operation\n"
        "  --dry-run                does not apply, only plans\n"
        "  --output <file>          output file for the script command\n"
        "  --all                    writes every migration from scratch in the script command\n"
        "  --namespace <name>       C++ namespace for scaffold (defaults to scaffold_models)\n"
        "\n"
        "rename-table command:\n"
        "  entityfy_migrate rename-table <old_table> --to <new_table> --db <uri>\n"
        "  Renames the table in place (ALTER TABLE ... RENAME TO ...).\n";
}

std::filesystem::path default_root(std::string_view provider)
{
    return std::filesystem::path{"migrations"} / std::string{provider};
}

arguments parse(int argc, char** argv)
{
    arguments output;
    if(argc < 2)
    {
        print_help();
        std::exit(0);
    }

    output.command = argv[1];

    for(int index = 2; index < argc; ++index)
    {
        const std::string value = argv[index];

        if(value == "--db" && index + 1 < argc)
        {
            output.database = argv[++index];
        }
        else if(value == "--provider" && index + 1 < argc)
        {
            output.provider = argv[++index];
        }
        else if(value == "--root" && index + 1 < argc)
        {
            output.root = argv[++index];
            output.root_explicit = true;
        }
        else if(value == "--rename" && index + 1 < argc)
        {
            const std::string mapping = argv[++index];
            const auto separator = mapping.find('=');
            if(separator == std::string::npos)
            {
                throw std::runtime_error{"entityfy-migrate: --rename format must be table.column=new_name"};
            }

            output.renames[mapping.substr(0, separator)] = mapping.substr(separator + 1);
        }
        else if(value == "--allow-destructive")
        {
            output.allow_destructive = true;
        }
        else if(value == "--force")
        {
            output.force = true;
        }
        else if(value == "--all")
        {
            output.all = true;
        }
        else if(value == "--no-backup")
        {
            output.backup = false;
        }
        else if(value == "--dry-run")
        {
            output.dry_run = true;
        }
        else if(value == "--output" && index + 1 < argc)
        {
            output.output_path = argv[++index];
        }
        else if(value == "--namespace" && index + 1 < argc)
        {
            output.namespace_name = argv[++index];
        }
        else if(value == "--to" && index + 1 < argc)
        {
            output.rename_to = argv[++index];
        }
        else if(!value.starts_with("--") && output.name.empty())
        {
            output.name = value;
        }
        else
        {
            throw std::runtime_error{"entityfy-migrate: unknown argument: " + value};
        }
    }

    if(output.provider.empty())
    {
        output.provider = "sqlite";
    }

    if(output.root.empty())
    {
        output.root = default_root(output.provider);
    }

    return output;
}

std::string provider_from_uri(const std::string& uri)
{
    if(uri.starts_with("postgres://") || uri.starts_with("postgresql://"))
    {
        return "postgres";
    }

    if(uri.starts_with("sqlite://"))
    {
        return "sqlite";
    }

    if(uri.empty())
    {
        return {};
    }

    return "sqlite";
}

std::string normalize_database_uri(std::string uri)
{
    if(uri.empty())
    {
        return "sqlite://app.db";
    }

    if(uri.find("://") == std::string::npos)
    {
        return "sqlite://" + uri;
    }

    return uri;
}

entityfy::dialect dialect_of(std::string_view provider)
{
    return provider == "postgres" ? entityfy::dialect::postgresql : entityfy::dialect::sqlite;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        auto options = parse(argc, argv);

        if(options.command == "help" || options.command == "--help")
        {
            print_help();
            return 0;
        }

        const std::string uri = normalize_database_uri(options.database);

        if(!options.database.empty())
        {
            const std::string detected = provider_from_uri(options.database);
            if(!detected.empty())
            {
                options.provider = detected;
            }
        }

        if(!options.root_explicit)
        {
            options.root = default_root(options.provider);
        }

        const entityfy::dialect target = dialect_of(options.provider);
        const entityfy::migrate::schema models = current_schema(target);

        // snapshot/add do not need a database; the engine only uses its name.
        std::unique_ptr<entityfy::migrate::provider> backend;
        if(options.command == "snapshot" || options.command == "add")
        {
            backend = entityfy::migrate::open_sqlite_provider(":memory:");
        }
        else
        {
            if(options.provider == "postgres")
            {
                backend = entityfy::migrate::open_postgres_provider(uri);
            }
            else
            {
                backend = entityfy::migrate::open_sqlite_provider(uri);
            }
        }

        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
            .root = options.root,
            .provider_name = options.provider,
            .allow_destructive = options.allow_destructive,
            .backup = options.backup,
            .dry_run = options.dry_run,
            .renames = options.renames,
        }};

        if(options.command == "rename-table")
        {
            if(options.name.empty() || options.rename_to.empty())
            {
                throw std::runtime_error{
                    "entityfy-migrate: rename-table needs a source table and a --to target"};
            }

            entityfy::migrate::rename_table(*backend, options.name, options.rename_to);
            std::cout << "table renamed: " << options.name << " -> " << options.rename_to << "\n";
            return 0;
        }

        if(options.command == "snapshot")
        {
            runner.write_snapshot(models);
            std::cout << "snapshot written: " << (options.root / "schema.snapshot.json").string() << "\n";
            return 0;
        }

        if(options.command == "add")
        {
            if(options.name.empty())
            {
                throw std::runtime_error{"entityfy-migrate: migration name required for add: entityfy_migrate add <name>"};
            }

            const auto file = runner.add(models, options.name);
            std::cout << "migration created: " << file.id << "\n";
            return 0;
        }

        if(options.command == "plan")
        {
            const auto report = runner.plan_only(models, std::cout);
            for(const auto& id: report.pending)
            {
                std::cout << "pending: " << id << "\n";
            }
            return 0;
        }

        if(options.command == "apply")
        {
            const auto report = runner.apply(models, std::cout);
            for(const auto& id: report.applied)
            {
                std::cout << "applied: " << id << "\n";
            }
            if(report.dry_run)
            {
                std::cout << "dry-run: nothing was applied\n";
            }
            return 0;
        }

        if(options.command == "status")
        {
            runner.status(models, std::cout);
            return 0;
        }

        if(options.command == "scaffold")
        {
            const entityfy::migrate::schema live = backend->introspect();
            const std::string generated = entityfy::migrate::generate_models(live, options.namespace_name);

            if(options.output_path.empty())
            {
                std::cout << generated;
                return 0;
            }

            std::ofstream stream{options.output_path, std::ios::binary | std::ios::trunc};
            if(!stream)
            {
                throw std::runtime_error{"entityfy-migrate: cannot write " + options.output_path};
            }

            stream << generated;
            std::cout << "scaffold written: " << options.output_path << "\n";
            return 0;
        }

        if(options.command == "script")
        {
            std::ofstream stream;

            if(!options.output_path.empty())
            {
                stream.open(options.output_path, std::ios::binary | std::ios::trunc);
                if(!stream)
                {
                    throw std::runtime_error{"entityfy-migrate: cannot write " + options.output_path};
                }
            }

            std::ostream& target = options.output_path.empty() ? std::cout : stream;

            if(options.all)
            {
                runner.script_all(target);
            }
            else
            {
                runner.script_pending(models, target);
            }

            if(!options.output_path.empty())
            {
                std::cout << "script written: " << options.output_path << "\n";
            }

            return 0;
        }

        if(options.command == "baseline")
        {
            runner.baseline(models, options.force, std::cout);
            return 0;
        }

        if(options.command == "rollback")
        {
            runner.rollback_last(std::cout);
            return 0;
        }

        print_help();
        return 0;
    }
    catch(const std::exception& error)
    {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
