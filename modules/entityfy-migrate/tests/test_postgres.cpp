#include "harness.hpp"

#include <entityfy/migrate/engine.hpp>
#include <entityfy/migrate/json.hpp>
#include <entityfy/migrate/provider.hpp>

#include "models.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace
{

void cleanup(entityfy::migrate::provider& backend)
{
    backend.execute_sql("DROP TABLE IF EXISTS \"posts\" CASCADE");
    backend.execute_sql("DROP TABLE IF EXISTS \"users\" CASCADE");
    backend.execute_sql("DROP TABLE IF EXISTS \"pg_migrate_pk_lines\" CASCADE");
    backend.execute_sql("DROP TABLE IF EXISTS \"entityfy_schema_migrations\" CASCADE");
}

void drop_composite_tables(entityfy::migrate::provider& backend)
{
    backend.execute_sql("DROP TABLE IF EXISTS \"pg_migrate_ck_lines\" CASCADE");
    backend.execute_sql("DROP TABLE IF EXISTS \"pg_migrate_ck_notes\" CASCADE");
}

void apply_plan(entityfy::migrate::provider& backend, const entityfy::migrate::plan& value)
{
    backend.start();
    try
    {
        for(const auto& current: value.steps)
        {
            backend.execute(current);
        }

        backend.finish();
    }
    catch(...)
    {
        backend.abort();
        throw;
    }
}

std::string history_count(entityfy::migrate::provider& backend)
{
    const auto rows = backend.query("SELECT COUNT(*) FROM \"entityfy_schema_migrations\"");
    return rows.empty() || rows.front().empty() ? "0" : rows.front().front();
}

} // namespace

entityfy_test::status test_postgres()
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr || std::string{uri}.empty())
    {
        const char* require_pg = std::getenv("ENTITYFY_REQUIRE_PG");
        const bool required = require_pg != nullptr && std::string{require_pg} == "1";

        if(required)
        {
            throw std::runtime_error{"ENTITYFY_REQUIRE_PG=1 but ENTITYFY_PG_URI is not set"};
        }

        std::cout << "postgres skipped: ENTITYFY_PG_URI is not set\n";
        return entityfy_test::status::skipped;
    }

    auto backend = entityfy::migrate::open_postgres_provider(uri);
    cleanup(*backend);

    const auto root = test_workspace("entityfy_pg_test_migrations");
    std::filesystem::remove_all(root);

    const entityfy::migrate::schema initial = current_schema(entityfy::dialect::postgresql);

    const entityfy::migrate::schema v2 = [&] {
        auto value = initial;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{.name = "nickname", .type = "VARCHAR(40)", .not_null = true,
                        .default_sql = "'yok'"});
        return value;
    }();

    const entityfy::migrate::schema v3 = [&] {
        auto value = v2;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(value, "users"), "name")->name = "full_name";
        return value;
    }();

    const entityfy::migrate::schema v4 = [&] {
        auto value = v3;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(value, "users"), "email")->type = "VARCHAR(100)";
        return value;
    }();

    const entityfy::migrate::schema v5 = [&] {
        auto value = v4;
        entityfy::migrate::find_table(value, "users")->columns.push_back(
            entityfy::migrate::column{.name = "required_code", .type = "VARCHAR(10)", .not_null = true});
        return value;
    }();

    std::ostringstream output;

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "postgres",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(initial, "init");
        runner.apply(initial, output);
        require(entityfy::migrate::same_schema(initial, backend->introspect()), "pg initial apply matches model");

        backend->execute_sql("INSERT INTO \"users\" (\"email\", \"name\") VALUES ('ada@test.com', 'Ada')");
        backend->execute_sql(
            "INSERT INTO \"posts\" (\"user_id\", \"title\", \"body\", \"published\") VALUES (1, 'p', 'b', true)");

        (void)runner.add(v2, "nickname");
        runner.apply(v2, output);
        require(entityfy::migrate::same_schema(v2, backend->introspect()), "pg add column applied");

        auto rows = backend->query("SELECT \"nickname\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "yok", "pg default applied to existing row");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "postgres",
                                        .allow_destructive = false,
                                        .backup = false,
                                        .renames = {{"users.name", "full_name"}},
                                    }};

        (void)runner.add(v3, "rename_name");
        runner.apply(v3, output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "pg rename applied");

        auto rows = backend->query("SELECT \"full_name\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "Ada", "pg data preserved after rename");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "postgres",
                                        .allow_destructive = false,
                                        .backup = false,
                                    }};

        (void)runner.add(v4, "email_shorter");

        bool refused = false;
        try
        {
            runner.apply(v4, output);
        }
        catch(const std::exception&)
        {
            refused = true;
        }

        require(refused, "pg destructive migration refused by default");
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "pg database unchanged after refusal");
    }

    {
        entityfy::migrate::engine runner{*backend, entityfy::migrate::engine_options{
                                        .root = root,
                                        .provider_name = "postgres",
                                        .allow_destructive = true,
                                        .backup = false,
                                    }};

        runner.apply(v4, output);
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "pg type change applied");

        auto rows = backend->query("SELECT \"email\" FROM \"users\"");
        require(rows.size() == 1 && rows.front().front() == "ada@test.com", "pg data preserved after type change");

        // NOT NULL column without a default: PostgreSQL errors, so the transaction must be rolled back.
        (void)runner.add(v5, "required_code");
        const std::string before = history_count(*backend);

        bool failed = false;
        try
        {
            runner.apply(v5, output);
        }
        catch(const std::exception&)
        {
            failed = true;
        }

        require(failed, "pg invalid NOT NULL migration failed");
        require(history_count(*backend) == before, "pg history rolled back");
        require(entityfy::migrate::same_schema(v4, backend->introspect()), "pg schema rolled back after failure");

        runner.rollback_last(output);
        require(entityfy::migrate::same_schema(v3, backend->introspect()), "pg rollback restores previous schema");

        runner.status(v3, output);
        require(output.str().find("in sync") != std::string::npos, "pg status reports in sync");
    }

    backend->execute_sql("DELETE FROM \"users\" WHERE \"id\" = 1");
    const auto remaining = backend->query("SELECT COUNT(*) FROM \"posts\"");
    require(remaining.size() == 1 && remaining.front().front() == "0", "pg foreign key cascade works");

    // Composite keys use a table-level PRIMARY KEY and must round-trip; a
    // single-column key keeps its column-level form.
    {
        drop_composite_tables(*backend);

        entityfy::migrate::schema composite;

        entityfy::migrate::table lines;
        lines.name = "pg_migrate_ck_lines";
        lines.primary_key = {"id", "region"};
        lines.columns.push_back(
            entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true});
        lines.columns.push_back(
            entityfy::migrate::column{.name = "region", .type = "VARCHAR(16)", .not_null = true, .primary_key = true});
        lines.columns.push_back(entityfy::migrate::column{.name = "quantity", .type = "INTEGER", .not_null = true});
        composite.tables.push_back(lines);

        entityfy::migrate::table notes;
        notes.name = "pg_migrate_ck_notes";
        notes.primary_key = {"id"};
        notes.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true,
                                            .primary_key = true, .auto_increment = true});
        notes.columns.push_back(entityfy::migrate::column{.name = "body", .type = "TEXT", .not_null = true});
        composite.tables.push_back(notes);

        const auto creation = backend->make_plan(composite, entityfy::migrate::schema{}, {});
        require(creation.steps.size() == 2, "pg composite creation creates both tables");
        require(!creation.destructive, "pg composite creation is not destructive");
        apply_plan(*backend, creation);

        const auto actual = backend->introspect();

        entityfy::migrate::schema subset;
        for(const auto* name: {"pg_migrate_ck_lines", "pg_migrate_ck_notes"})
        {
            if(const auto* found = entityfy::migrate::find_table(actual, name))
            {
                subset.tables.push_back(*found);
            }
        }

        require(subset.tables.size() == 2, "pg composite tables introspected");
        if(!entityfy::migrate::same_schema(composite, subset))
        {
            std::cout << "desired:\n" << entityfy::migrate::schema_to_json(composite) << "\nactual:\n"
                      << entityfy::migrate::schema_to_json(subset) << "\n";
        }
        require(entityfy::migrate::same_schema(composite, subset), "pg composite create table roundtrip");

        const auto* actual_lines = entityfy::migrate::find_table(subset, "pg_migrate_ck_lines");
        require(actual_lines != nullptr &&
                    actual_lines->primary_key == std::vector<std::string>{"id", "region"},
                "pg composite key order introspected");

        const auto* actual_notes = entityfy::migrate::find_table(subset, "pg_migrate_ck_notes");
        require(actual_notes != nullptr &&
                    entityfy::migrate::primary_key_columns(*actual_notes) == std::vector<std::string>{"id"},
                "pg single key column introspected");

        const auto idempotent = backend->make_plan(composite, subset, {});
        require(idempotent.steps.empty(), "pg second composite apply has no steps");
        require(!idempotent.destructive, "pg idempotent composite plan is not destructive");

        backend->execute_sql(
            "INSERT INTO \"pg_migrate_ck_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'eu', 3)");

        bool duplicate_rejected = false;
        try
        {
            backend->execute_sql(
                "INSERT INTO \"pg_migrate_ck_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'eu', 9)");
        }
        catch(const std::exception&)
        {
            duplicate_rejected = true;
        }

        require(duplicate_rejected, "pg composite key rejects duplicate rows");

        backend->execute_sql(
            "INSERT INTO \"pg_migrate_ck_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'us', 4)");
        const auto counted = backend->query("SELECT COUNT(*) FROM \"pg_migrate_ck_lines\"");
        require(counted.size() == 1 && counted.front().front() == "2", "pg different regions are distinct keys");

        drop_composite_tables(*backend);

        const auto after_drop = backend->introspect();
        require(entityfy::migrate::find_table(after_drop, "pg_migrate_ck_lines") == nullptr &&
                    entityfy::migrate::find_table(after_drop, "pg_migrate_ck_notes") == nullptr,
                "pg composite tables dropped");
    }

    // Changing which columns make up an existing primary key is a constraint
    // swap: the old key is dropped, the column steps run, and the new key is
    // added at the end.
    {
        backend->execute_sql("DROP TABLE IF EXISTS \"pg_migrate_pk_lines\" CASCADE");

        const auto subset_of = [](const entityfy::migrate::schema& source, std::string_view name) {
            entityfy::migrate::schema output;
            if(const auto* found = entityfy::migrate::find_table(source, name))
            {
                output.tables.push_back(*found);
            }
            return output;
        };

        entityfy::migrate::schema single;

        entityfy::migrate::table lines;
        lines.name = "pg_migrate_pk_lines";
        lines.primary_key = {"id"};
        lines.columns.push_back(
            entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true, .primary_key = true});
        lines.columns.push_back(entityfy::migrate::column{.name = "region", .type = "VARCHAR(16)", .not_null = true});
        lines.columns.push_back(entityfy::migrate::column{.name = "quantity", .type = "INTEGER", .not_null = true});
        single.tables.push_back(lines);

        const auto creation = backend->make_plan(single, entityfy::migrate::schema{}, {});
        require(creation.steps.size() == 1, "pg key test creates the table with its key");
        apply_plan(*backend, creation);

        backend->execute_sql(
            "INSERT INTO \"pg_migrate_pk_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'eu', 3)");
        backend->execute_sql(
            "INSERT INTO \"pg_migrate_pk_lines\" (\"id\", \"region\", \"quantity\") VALUES (2, 'us', 4)");

        const auto created = subset_of(backend->introspect(), "pg_migrate_pk_lines");
        const auto* created_lines = entityfy::migrate::find_table(created, "pg_migrate_pk_lines");
        require(created_lines != nullptr &&
                    entityfy::migrate::primary_key_columns(*created_lines) == std::vector<std::string>{"id"},
                "pg key test starts from a single-column key");

        const auto constraint_rows = backend->query_params(
            "SELECT c.conname FROM pg_constraint c "
            "JOIN pg_class t ON t.oid = c.conrelid "
            "JOIN pg_namespace n ON n.oid = t.relnamespace "
            "WHERE c.contype = 'p' AND n.nspname = current_schema() AND t.relname = $1",
            {"pg_migrate_pk_lines"});
        require(constraint_rows.size() == 1 && !constraint_rows.front().empty(),
                "pg key test reads the constraint name from the catalog");

        auto promoted = single;
        auto* promoted_lines = entityfy::migrate::find_table(promoted, "pg_migrate_pk_lines");
        promoted_lines->primary_key = {"id", "region"};
        entityfy::migrate::find_column(*promoted_lines, "region")->primary_key = true;

        const auto promote = backend->make_plan(promoted, created, {});
        require(promote.destructive, "pg primary key change is destructive");
        require(promote.steps.size() == 2, "pg primary key change plans a drop and an add");
        require(promote.steps.front().sql ==
                    "ALTER TABLE \"pg_migrate_pk_lines\" DROP CONSTRAINT \"" + constraint_rows.front().front() + "\"",
                "pg primary key change drops the existing constraint first");
        require(promote.steps.back().sql == "ALTER TABLE \"pg_migrate_pk_lines\" ADD PRIMARY KEY (\"id\", \"region\")",
                "pg primary key change adds the new key in declaration order");
        apply_plan(*backend, promote);

        const auto after_promote = subset_of(backend->introspect(), "pg_migrate_pk_lines");
        const auto* promoted_actual = entityfy::migrate::find_table(after_promote, "pg_migrate_pk_lines");
        require(promoted_actual != nullptr &&
                    entityfy::migrate::primary_key_columns(*promoted_actual) ==
                        std::vector<std::string>{"id", "region"},
                "pg promoted primary key introspected in order");
        require(entityfy::migrate::same_schema(promoted, after_promote),
                "pg promoted primary key matches the model");

        const auto promoted_rows = backend->query(
            "SELECT \"id\", \"region\", \"quantity\" FROM \"pg_migrate_pk_lines\" ORDER BY \"id\"");
        require(promoted_rows.size() == 2 && promoted_rows.front().size() == 3 &&
                    promoted_rows.front()[1] == "eu" && promoted_rows.back()[1] == "us" &&
                    promoted_rows.back()[2] == "4",
                "pg rows survive the primary key change");

        bool promoted_rejects_duplicate = false;
        try
        {
            backend->execute_sql(
                "INSERT INTO \"pg_migrate_pk_lines\" (\"id\", \"region\", \"quantity\") VALUES (1, 'eu', 9)");
        }
        catch(const std::exception&)
        {
            promoted_rejects_duplicate = true;
        }

        require(promoted_rejects_duplicate, "pg promoted key is enforced");

        // A type change on a key column alters the column and leaves the key in place.
        auto widened = promoted;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(widened, "pg_migrate_pk_lines"), "region")->type =
            "VARCHAR(32)";

        const auto widen = backend->make_plan(widened, after_promote, {});
        require(widen.destructive, "pg key column type change is destructive");
        require(widen.steps.size() == 1 &&
                    widen.steps.front().sql.find("CONSTRAINT") == std::string::npos,
                "pg key column type change keeps the key constraint");
        apply_plan(*backend, widen);

        const auto after_widen = subset_of(backend->introspect(), "pg_migrate_pk_lines");
        const auto* widened_actual = entityfy::migrate::find_table(after_widen, "pg_migrate_pk_lines");
        require(widened_actual != nullptr &&
                    entityfy::migrate::primary_key_columns(*widened_actual) ==
                        std::vector<std::string>{"id", "region"},
                "pg key survives a key column type change");
        require(entityfy::migrate::same_schema(widened, after_widen),
                "pg widened key column matches the model");

        // Going back to the single-column key swaps the constraint again.
        auto demoted = single;
        entityfy::migrate::find_column(*entityfy::migrate::find_table(demoted, "pg_migrate_pk_lines"), "region")->type =
            "VARCHAR(32)";

        const auto demote = backend->make_plan(demoted, after_widen, {});
        require(demote.destructive, "pg primary key narrowing is destructive");
        require(demote.steps.size() == 2 &&
                    demote.steps.back().sql == "ALTER TABLE \"pg_migrate_pk_lines\" ADD PRIMARY KEY (\"id\")",
                "pg primary key narrowing re-adds the single-column key");
        apply_plan(*backend, demote);

        const auto after_demote = subset_of(backend->introspect(), "pg_migrate_pk_lines");
        const auto* demoted_actual = entityfy::migrate::find_table(after_demote, "pg_migrate_pk_lines");
        require(demoted_actual != nullptr &&
                    entityfy::migrate::primary_key_columns(*demoted_actual) == std::vector<std::string>{"id"},
                "pg narrowed primary key introspected");
        require(entityfy::migrate::same_schema(demoted, after_demote),
                "pg narrowed primary key matches the model");

        const auto counted = backend->query("SELECT COUNT(*) FROM \"pg_migrate_pk_lines\"");
        require(counted.size() == 1 && counted.front().front() == "2", "pg rows survive the key narrowing");

        // A change confined to the table-level key list produces no column
        // operation, but the constraint still has to be swapped.
        auto listed = demoted;
        entityfy::migrate::find_table(listed, "pg_migrate_pk_lines")->primary_key = {"id", "quantity"};

        const auto relist = backend->make_plan(listed, after_demote, {});
        require(relist.destructive, "pg table-level key list change is destructive");
        require(relist.steps.size() == 2 &&
                    relist.steps.back().sql ==
                        "ALTER TABLE \"pg_migrate_pk_lines\" ADD PRIMARY KEY (\"id\", \"quantity\")",
                "pg table-level key list change still swaps the constraint");
        apply_plan(*backend, relist);

        const auto after_relist = subset_of(backend->introspect(), "pg_migrate_pk_lines");
        const auto* relisted_actual = entityfy::migrate::find_table(after_relist, "pg_migrate_pk_lines");
        require(relisted_actual != nullptr &&
                    entityfy::migrate::primary_key_columns(*relisted_actual) ==
                        std::vector<std::string>{"id", "quantity"},
                "pg table-level key list applied");

        const auto relisted_rows = backend->query("SELECT COUNT(*) FROM \"pg_migrate_pk_lines\"");
        require(relisted_rows.size() == 1 && relisted_rows.front().front() == "2",
                "pg rows survive the table-level key list change");

        const auto relist_again = backend->make_plan(listed, after_relist, {});
        require(relist_again.steps.empty(), "pg table-level key list apply is idempotent");

        backend->execute_sql("DROP TABLE IF EXISTS \"pg_migrate_pk_lines\" CASCADE");
        require(entityfy::migrate::find_table(backend->introspect(), "pg_migrate_pk_lines") == nullptr,
                "pg key test table dropped");
    }

    // Composite foreign keys are one table-level constraint: create, read it
    // back as one entry with ordered lists, drop by catalog name, re-add, and
    // prove both the pairing and the cascade survive.
    {
        backend->execute_sql("DROP TABLE IF EXISTS \"pg_migrate_fk_children\" CASCADE");
        backend->execute_sql("DROP TABLE IF EXISTS \"pg_migrate_fk_parents\" CASCADE");

        entityfy::migrate::schema composite;

        entityfy::migrate::table parents;
        parents.name = "pg_migrate_fk_parents";
        parents.primary_key = {"tenant_id", "code"};
        parents.columns.push_back(entityfy::migrate::column{.name = "tenant_id", .type = "INTEGER", .not_null = true,
                                                            .primary_key = true});
        parents.columns.push_back(entityfy::migrate::column{.name = "code", .type = "VARCHAR(16)", .not_null = true,
                                                            .primary_key = true});
        parents.columns.push_back(entityfy::migrate::column{.name = "name", .type = "VARCHAR(80)", .not_null = true});
        composite.tables.push_back(parents);

        entityfy::migrate::table children;
        children.name = "pg_migrate_fk_children";
        children.primary_key = {"id"};
        children.columns.push_back(entityfy::migrate::column{.name = "id", .type = "INTEGER", .not_null = true,
                                                             .primary_key = true, .auto_increment = true});
        children.columns.push_back(entityfy::migrate::column{.name = "tenant_id", .type = "INTEGER", .not_null = true});
        children.columns.push_back(entityfy::migrate::column{.name = "parent_code", .type = "VARCHAR(16)", .not_null = true});
        children.columns.push_back(entityfy::migrate::column{.name = "body", .type = "VARCHAR(80)", .not_null = true});
        children.foreign_keys.push_back(entityfy::migrate::foreign_key{
            .column = "tenant_id",
            .ref_table = "pg_migrate_fk_parents",
            .ref_column = "tenant_id",
            .on_delete = "CASCADE",
            .on_update = "CASCADE",
            .columns = {"tenant_id", "parent_code"},
            .ref_columns = {"tenant_id", "code"},
        });
        composite.tables.push_back(children);

        const auto creation = backend->make_plan(composite, entityfy::migrate::schema{}, {});
        require(creation.steps.size() == 2, "pg composite fk creation creates both tables");
        require(!creation.destructive, "pg composite fk creation is not destructive");

        const auto create_child = std::ranges::find_if(creation.steps, [](const entityfy::migrate::step& value) {
            return value.sql.starts_with("CREATE TABLE") &&
                   value.sql.find("\"pg_migrate_fk_children\"") != std::string::npos;
        });
        require(create_child != creation.steps.end() &&
                    create_child->sql.find(
                        "CONSTRAINT \"fk_pg_migrate_fk_children_tenant_id_parent_code\" "
                        "FOREIGN KEY (\"tenant_id\", \"parent_code\") REFERENCES \"pg_migrate_fk_parents\" "
                        "(\"tenant_id\", \"code\") ON DELETE CASCADE ON UPDATE CASCADE") != std::string::npos,
                "pg create table emits one named composite foreign key");

        apply_plan(*backend, creation);

        const auto subset_of = [](const entityfy::migrate::schema& source,
                                  std::initializer_list<std::string_view> names) {
            entityfy::migrate::schema output;
            for(const auto name: names)
            {
                if(const auto* found = entityfy::migrate::find_table(source, name))
                {
                    output.tables.push_back(*found);
                }
            }
            return output;
        };

        const auto actual = subset_of(backend->introspect(),
                                      {"pg_migrate_fk_parents", "pg_migrate_fk_children"});
        require(actual.tables.size() == 2, "pg composite fk tables introspected");

        const auto* child = entityfy::migrate::find_table(actual, "pg_migrate_fk_children");
        require(child != nullptr && child->foreign_keys.size() == 1,
                "pg composite foreign key introspected as one constraint");
        require(child->foreign_keys.front().columns == std::vector<std::string>{"tenant_id", "parent_code"},
                "pg composite foreign key local columns introspected");
        require(child->foreign_keys.front().ref_columns == std::vector<std::string>{"tenant_id", "code"},
                "pg composite foreign key referenced columns introspected");
        require(child->foreign_keys.front().on_delete == "CASCADE" &&
                    child->foreign_keys.front().on_update == "CASCADE",
                "pg composite foreign key actions introspected");
        require(!entityfy::migrate::find_column(*child, "tenant_id")->references.has_value() &&
                    !entityfy::migrate::find_column(*child, "parent_code")->references.has_value(),
                "pg composite columns have no column-level reference");

        const auto* parent = entityfy::migrate::find_table(actual, "pg_migrate_fk_parents");
        require(parent != nullptr && parent->foreign_keys.empty(), "pg parent has no foreign keys");
        require(entityfy::migrate::same_schema(composite, actual), "pg composite fk create roundtrip");

        const auto idempotent = backend->make_plan(composite, actual, {});
        require(idempotent.steps.empty(), "pg composite foreign key apply is idempotent");

        backend->execute_sql(
            "INSERT INTO \"pg_migrate_fk_parents\" (\"tenant_id\", \"code\", \"name\") VALUES (1, 'eu', 'EU')");
        backend->execute_sql(
            "INSERT INTO \"pg_migrate_fk_parents\" (\"tenant_id\", \"code\", \"name\") VALUES (1, 'us', 'US')");
        backend->execute_sql(
            "INSERT INTO \"pg_migrate_fk_parents\" (\"tenant_id\", \"code\", \"name\") VALUES (2, 'eu', 'DE')");
        backend->execute_sql(
            "INSERT INTO \"pg_migrate_fk_children\" (\"id\", \"tenant_id\", \"parent_code\", \"body\") "
            "VALUES (1, 1, 'eu', 'first')");
        backend->execute_sql(
            "INSERT INTO \"pg_migrate_fk_children\" (\"id\", \"tenant_id\", \"parent_code\", \"body\") "
            "VALUES (2, 1, 'us', 'second')");
        backend->execute_sql(
            "INSERT INTO \"pg_migrate_fk_children\" (\"id\", \"tenant_id\", \"parent_code\", \"body\") "
            "VALUES (3, 2, 'eu', 'third')");

        bool mismatch_rejected = false;
        try
        {
            backend->execute_sql(
                "INSERT INTO \"pg_migrate_fk_children\" (\"id\", \"tenant_id\", \"parent_code\", \"body\") "
                "VALUES (4, 1, 'zz', 'mismatch')");
        }
        catch(const std::exception&)
        {
            mismatch_rejected = true;
        }

        require(mismatch_rejected, "pg composite foreign key enforces the column pair");

        backend->execute_sql(
            "DELETE FROM \"pg_migrate_fk_parents\" WHERE \"tenant_id\" = 1 AND \"code\" = 'eu'");
        auto counted = backend->query("SELECT COUNT(*) FROM \"pg_migrate_fk_children\"");
        require(counted.size() == 1 && counted.front().front() == "2", "pg composite foreign key cascades");

        auto without = composite;
        entityfy::migrate::find_table(without, "pg_migrate_fk_children")->foreign_keys.clear();

        const auto drop_plan = backend->make_plan(without, actual, {});
        require(!drop_plan.destructive, "pg composite foreign key drop is not destructive");
        require(drop_plan.steps.size() == 1 &&
                    drop_plan.steps.front().sql ==
                        "ALTER TABLE \"pg_migrate_fk_children\" DROP CONSTRAINT "
                        "\"fk_pg_migrate_fk_children_tenant_id_parent_code\"",
                "pg composite foreign key dropped by its catalog name");
        apply_plan(*backend, drop_plan);

        const auto after_drop = subset_of(backend->introspect(),
                                          {"pg_migrate_fk_parents", "pg_migrate_fk_children"});
        const auto* dropped_child = entityfy::migrate::find_table(after_drop, "pg_migrate_fk_children");
        require(dropped_child != nullptr && dropped_child->foreign_keys.empty(),
                "pg composite foreign key dropped");
        require(entityfy::migrate::same_schema(without, after_drop),
                "pg composite foreign key drop matches the model");

        counted = backend->query("SELECT COUNT(*) FROM \"pg_migrate_fk_children\"");
        require(counted.size() == 1 && counted.front().front() == "2",
                "pg child rows survive dropping the composite foreign key");
        counted = backend->query("SELECT COUNT(*) FROM \"pg_migrate_fk_parents\"");
        require(counted.size() == 1 && counted.front().front() == "2",
                "pg parent rows survive dropping the child constraint");

        const auto readd = backend->make_plan(composite, after_drop, {});
        require(readd.steps.size() == 1 &&
                    readd.steps.front().sql ==
                        "ALTER TABLE \"pg_migrate_fk_children\" ADD CONSTRAINT "
                        "\"fk_pg_migrate_fk_children_tenant_id_parent_code\" "
                        "FOREIGN KEY (\"tenant_id\", \"parent_code\") REFERENCES \"pg_migrate_fk_parents\" "
                        "(\"tenant_id\", \"code\") ON DELETE CASCADE ON UPDATE CASCADE",
                "pg composite foreign key re-added as one constraint");
        apply_plan(*backend, readd);

        const auto after_readd = subset_of(backend->introspect(),
                                           {"pg_migrate_fk_parents", "pg_migrate_fk_children"});
        const auto* readded_child = entityfy::migrate::find_table(after_readd, "pg_migrate_fk_children");
        require(readded_child != nullptr && readded_child->foreign_keys.size() == 1 &&
                    readded_child->foreign_keys.front().columns ==
                        std::vector<std::string>{"tenant_id", "parent_code"} &&
                    readded_child->foreign_keys.front().ref_columns ==
                        std::vector<std::string>{"tenant_id", "code"},
                "pg composite foreign key re-added with both column lists");

        counted = backend->query("SELECT COUNT(*) FROM \"pg_migrate_fk_children\"");
        require(counted.size() == 1 && counted.front().front() == "2",
                "pg child rows survive re-adding the composite foreign key");

        bool enforced_again = false;
        try
        {
            backend->execute_sql(
                "INSERT INTO \"pg_migrate_fk_children\" (\"id\", \"tenant_id\", \"parent_code\", \"body\") "
                "VALUES (5, 2, 'zz', 'mismatch')");
        }
        catch(const std::exception&)
        {
            enforced_again = true;
        }

        require(enforced_again, "pg re-added composite foreign key is enforced");

        backend->execute_sql("DELETE FROM \"pg_migrate_fk_parents\" WHERE \"tenant_id\" = 2");
        counted = backend->query("SELECT COUNT(*) FROM \"pg_migrate_fk_children\"");
        require(counted.size() == 1 && counted.front().front() == "1",
                "pg re-added composite foreign key cascades");

        const auto stable = backend->make_plan(composite, after_readd, {});
        require(stable.steps.empty(), "pg composite foreign key re-add is idempotent");

        backend->execute_sql("DROP TABLE IF EXISTS \"pg_migrate_fk_children\" CASCADE");
        backend->execute_sql("DROP TABLE IF EXISTS \"pg_migrate_fk_parents\" CASCADE");
        require(entityfy::migrate::find_table(backend->introspect(), "pg_migrate_fk_children") == nullptr &&
                    entityfy::migrate::find_table(backend->introspect(), "pg_migrate_fk_parents") == nullptr,
                "pg composite fk tables dropped");
    }

    cleanup(*backend);

    return entityfy_test::status::passed;
}
