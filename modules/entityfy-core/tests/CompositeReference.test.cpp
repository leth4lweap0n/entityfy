#include <catch2/catch_test_macros.hpp>

#include <entityfy/entityfy.hpp>

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

// Composite key parent: the referenced pair (tenant_id, code) is the key, so a
// child can only be a real reference when both parts match.
struct [[= entityfy::table{"cr_parents"}]] cr_parent
{
    [[= entityfy::primary_key]]
    std::int64_t tenant_id = 0;

    [[= entityfy::primary_key, = entityfy::varchar{16}]]
    std::string code;

    [[= entityfy::not_null, = entityfy::varchar{80}]]
    std::string name;
};

// Both columns carry group 1: they form one composite foreign key.
struct [[= entityfy::table{"cr_children"}]] cr_child
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::references{"cr_parents", "tenant_id", "CASCADE", "CASCADE", 1}]]
    std::int64_t tenant_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{16}, = entityfy::references{"cr_parents", "code", "CASCADE", "CASCADE", 1}]]
    std::string parent_code;

    [[= entityfy::not_null, = entityfy::varchar{80}]]
    std::string name;
};

// Two independent groups on one model: one relation per group, each created
// where its first column appears.
struct [[= entityfy::table{"cr_two_links"}]] cr_two_links
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::references{"cr_parents", "tenant_id", "", "", 1}]]
    std::int64_t tenant_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{16}, = entityfy::references{"cr_parents", "code", "", "", 1}]]
    std::string parent_code;

    [[= entityfy::not_null, = entityfy::references{"cr_parents", "tenant_id", "", "", 2}]]
    std::int64_t backup_tenant_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{16}, = entityfy::references{"cr_parents", "code", "", "", 2}]]
    std::string backup_parent_code;
};

// A plain single-column reference: group 0 keeps the column-level clause.
struct [[= entityfy::table{"cr_singles"}]] cr_single
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::references{"cr_parents", "code", "RESTRICT", "RESTRICT"}]]
    std::string parent_code;

    [[= entityfy::not_null, = entityfy::varchar{80}]]
    std::string name;
};

// A single-column reference to a unique key. `cr_singles` points at a column
// that is only unique together with `tenant_id`, which PostgreSQL refuses to
// reference at all; this model gives every backend a valid single-column key.
struct [[= entityfy::table{"cr_notes"}]] cr_note
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::references{"cr_children", "id", "CASCADE", "CASCADE"}]]
    std::int64_t child_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{80}]]
    std::string body;
};

// RESTRICT spelled as T-SQL's NO ACTION proves the table-level clause reuses
// the single-column action mapping.
struct [[= entityfy::table{"cr_guarded"}]] cr_guarded
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::references{"cr_parents", "tenant_id", "RESTRICT", "RESTRICT", 3}]]
    std::int64_t tenant_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{16}, = entityfy::references{"cr_parents", "code", "RESTRICT", "RESTRICT", 3}]]
    std::string parent_code;
};

// A group exists for composite keys only; a single member must be rejected.
struct [[= entityfy::table{"cr_bad_group"}]] cr_bad_group
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::references{"cr_parents", "tenant_id", "", "", 4}]]
    std::int64_t tenant_id = 0;
};

// Every column of a group has to agree on the referential action.
struct [[= entityfy::table{"cr_mixed_actions"}]] cr_mixed_actions
{
    [[= entityfy::primary_key]]
    std::int64_t id = 0;

    [[= entityfy::not_null, = entityfy::references{"cr_parents", "tenant_id", "CASCADE", "", 5}]]
    std::int64_t tenant_id = 0;

    [[= entityfy::not_null, = entityfy::varchar{16}, = entityfy::references{"cr_parents", "code", "RESTRICT", "", 5}]]
    std::string parent_code;
};

[[nodiscard]] entityfy::row_key parent_key(std::int64_t tenant_id, std::string code)
{
    return entityfy::row_key{entityfy::sql_value{tenant_id}, entityfy::sql_value{std::move(code)}};
}

// The helpers throw before touching the database, so the message is collected
// in one place and asserted by the caller.
template <typename Function>
[[nodiscard]] std::string rejection_message(Function&& function)
{
    try
    {
        std::forward<Function>(function)();
    }
    catch(const std::logic_error& error)
    {
        return std::string{error.what()};
    }

    return {};
}

[[nodiscard]] std::string postgres_uri()
{
    const char* uri = std::getenv("ENTITYFY_PG_URI");
    if(uri == nullptr || *uri == '\0')
    {
        if(std::getenv("ENTITYFY_REQUIRE_PG") != nullptr)
        {
            throw std::runtime_error{"ENTITYFY_REQUIRE_PG is set but ENTITYFY_PG_URI is missing"};
        }

        return {};
    }

    return uri;
}

void drop_composite_reference_tables(entityfy::client& db)
{
    // Child first: PostgreSQL refuses to drop a referenced table otherwise.
    (void)db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"cr_notes\""});
    (void)db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"cr_singles\""});
    (void)db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"cr_children\""});
    (void)db.execute(entityfy::statement{.sql = "DROP TABLE IF EXISTS \"cr_parents\""});
}

// Exercises composite navigation on a migrated and empty parent/child pair.
// The fixture repeats one code in two tenants and uses two codes in one tenant,
// so a helper that only matched one key part would return the wrong rows.
void require_composite_relationship_navigation(entityfy::client& db)
{
    REQUIRE(db.insert(cr_parent{.tenant_id = 1, .code = "eu", .name = "europe"}).rows_affected == 1);
    REQUIRE(db.insert(cr_parent{.tenant_id = 1, .code = "us", .name = "americas"}).rows_affected == 1);
    REQUIRE(db.insert(cr_parent{.tenant_id = 2, .code = "eu", .name = "eu-west"}).rows_affected == 1);
    REQUIRE(db.insert(cr_parent{.tenant_id = 2, .code = "us", .name = "us-west"}).rows_affected == 1);

    REQUIRE(db.insert(cr_child{.id = 10, .tenant_id = 1, .parent_code = "eu", .name = "first"}).rows_affected == 1);
    REQUIRE(db.insert(cr_child{.id = 11, .tenant_id = 1, .parent_code = "us", .name = "second"}).rows_affected == 1);
    REQUIRE(db.insert(cr_child{.id = 12, .tenant_id = 2, .parent_code = "eu", .name = "third"}).rows_affected == 1);

    // A child with only one matching key part does not exist: the whole
    // composite foreign key is enforced by the backend.
    REQUIRE_THROWS_AS(
        db.insert(cr_child{.id = 13, .tenant_id = 1, .parent_code = "apac", .name = "orphan"}),
        entityfy::foreign_key_violation
    );
    REQUIRE_THROWS_AS(
        db.insert(cr_child{.id = 14, .tenant_id = 3, .parent_code = "eu", .name = "orphan"}),
        entityfy::foreign_key_violation
    );

    const cr_parent eu_one{.tenant_id = 1, .code = "eu", .name = "europe"};
    const cr_parent us_one{.tenant_id = 1, .code = "us", .name = "americas"};
    const cr_parent eu_two{.tenant_id = 2, .code = "eu", .name = "eu-west"};
    const cr_parent empty{.tenant_id = 2, .code = "us", .name = "us-west"};

    auto parents  = db.table<cr_parent>();
    auto children = db.table<cr_child>();

    const auto eu_one_children = parents.has_many<cr_child>(eu_one, &cr_child::tenant_id, &cr_child::parent_code);
    REQUIRE(eu_one_children.size() == 1);
    REQUIRE(eu_one_children.front().id == 10);

    const auto us_one_children = parents.has_many<cr_child>(us_one, &cr_child::tenant_id, &cr_child::parent_code);
    REQUIRE(us_one_children.size() == 1);
    REQUIRE(us_one_children.front().id == 11);

    // Same code, different tenant: matching only the second key part would
    // return this parent's sibling instead.
    const auto eu_two_children = parents.has_many<cr_child>(eu_two, &cr_child::tenant_id, &cr_child::parent_code);
    REQUIRE(eu_two_children.size() == 1);
    REQUIRE(eu_two_children.front().id == 12);

    const auto eu_one_child = parents.has_one<cr_child>(eu_one, &cr_child::tenant_id, &cr_child::parent_code);
    REQUIRE(eu_one_child.has_value());
    REQUIRE(eu_one_child->id == 10);

    REQUIRE_FALSE(parents.has_one<cr_child>(empty, &cr_child::tenant_id, &cr_child::parent_code).has_value());

    const auto first = children.find(std::int64_t{10});
    REQUIRE(first.has_value());

    const auto first_parent = children.belongs_to<cr_parent>(*first, &cr_child::tenant_id, &cr_child::parent_code);
    REQUIRE(first_parent.has_value());
    REQUIRE(first_parent->tenant_id == 1);
    REQUIRE(first_parent->code == "eu");

    const auto third = children.find(std::int64_t{12});
    REQUIRE(third.has_value());

    const auto third_parent = children.belongs_to<cr_parent>(*third, &cr_child::tenant_id, &cr_child::parent_code);
    REQUIRE(third_parent.has_value());
    REQUIRE(third_parent->tenant_id == 2);
    REQUIRE(third_parent->code == "eu");

    // A pointer count that does not match the parent key must not fall back to
    // a partial match; it names the operation and the expected column count.
    const auto has_many_message = rejection_message([&] {
        (void)parents.has_many<cr_child>(eu_one, &cr_child::tenant_id, &cr_child::parent_code, &cr_child::id);
    });
    INFO(has_many_message);
    REQUIRE(has_many_message.find("has_many") != std::string::npos);
    REQUIRE(has_many_message.find("expects 2") != std::string::npos);

    const auto has_one_message = rejection_message([&] {
        (void)parents.has_one<cr_child>(eu_one, &cr_child::tenant_id, &cr_child::parent_code, &cr_child::id);
    });
    INFO(has_one_message);
    REQUIRE(has_one_message.find("has_one") != std::string::npos);
    REQUIRE(has_one_message.find("expects 2") != std::string::npos);

    const auto belongs_to_message = rejection_message([&] {
        (void)children.belongs_to<cr_parent>(*first, &cr_child::tenant_id, &cr_child::parent_code, &cr_child::id);
    });
    INFO(belongs_to_message);
    REQUIRE(belongs_to_message.find("belongs_to") != std::string::npos);
    REQUIRE(belongs_to_message.find("expects 2") != std::string::npos);
}

} // namespace

TEST_CASE("a composite reference group produces one relation with ordered column lists", "[relations][composite]")
{
    const auto model = entityfy::describe_model<cr_child>(entityfy::dialect::sqlite);

    REQUIRE(model.relations.size() == 1);

    const auto& relation = model.relations.front();
    REQUIRE(relation.referenced_table == "cr_parents");
    REQUIRE(relation.on_delete == "CASCADE");
    REQUIRE(relation.on_update == "CASCADE");

    // Declaration order, not column name order, and not the order of the
    // referenced key.
    REQUIRE(relation.local_columns == std::vector<std::string>{"tenant_id", "parent_code"});
    REQUIRE(relation.referenced_columns == std::vector<std::string>{"tenant_id", "code"});

    // The single-column members repeat the first pair for older call sites.
    REQUIRE(relation.local_column == "tenant_id");
    REQUIRE(relation.referenced_column == "tenant_id");

    // The per-column descriptor keeps the group so statement generation can
    // suppress the column-level clause.
    const auto columns = entityfy::describe<cr_child>(entityfy::dialect::sqlite);
    REQUIRE(columns.size() == 4);
    REQUIRE(columns[1].has_reference);
    REQUIRE(columns[1].reference.group == 1);
    REQUIRE(columns[2].has_reference);
    REQUIRE(columns[2].reference.group == 1);
    REQUIRE_FALSE(columns[3].has_reference);
}

TEST_CASE("single-column references keep one-element relation lists", "[relations]")
{
    const auto model = entityfy::describe_model<cr_single>(entityfy::dialect::sqlite);

    REQUIRE(model.relations.size() == 1);

    const auto& relation = model.relations.front();
    REQUIRE(relation.local_column == "parent_code");
    REQUIRE(relation.referenced_table == "cr_parents");
    REQUIRE(relation.referenced_column == "code");
    REQUIRE(relation.on_delete == "RESTRICT");
    REQUIRE(relation.local_columns == std::vector<std::string>{"parent_code"});
    REQUIRE(relation.referenced_columns == std::vector<std::string>{"code"});

    const auto columns = entityfy::describe<cr_single>(entityfy::dialect::sqlite);
    REQUIRE(columns[1].reference.group == 0);
}

TEST_CASE("each composite group on a model becomes its own relation", "[relations][composite]")
{
    const auto model = entityfy::describe_model<cr_two_links>(entityfy::dialect::sqlite);

    REQUIRE(model.relations.size() == 2);
    REQUIRE(model.relations[0].local_columns == std::vector<std::string>{"tenant_id", "parent_code"});
    REQUIRE(model.relations[0].referenced_columns == std::vector<std::string>{"tenant_id", "code"});
    REQUIRE(model.relations[1].local_columns == std::vector<std::string>{"backup_tenant_id", "backup_parent_code"});
    REQUIRE(model.relations[1].referenced_columns == std::vector<std::string>{"tenant_id", "code"});
}

TEST_CASE("composite references emit one table-level foreign key", "[relations][composite][ddl]")
{
    const auto model = entityfy::describe_model<cr_child>(entityfy::dialect::sqlite);
    const auto create = entityfy::create_table(model);
    INFO(create.sql);

    REQUIRE(
        create.sql.find(
            "FOREIGN KEY (\"tenant_id\", \"parent_code\") REFERENCES \"cr_parents\" (\"tenant_id\", \"code\") "
            "ON DELETE CASCADE ON UPDATE CASCADE"
        ) != std::string::npos
    );

    // The constraint belongs to the table: the columns before it carry no
    // REFERENCES clause of their own, so the pair is emitted exactly once.
    const auto clause = create.sql.find(", FOREIGN KEY (");
    REQUIRE(clause != std::string::npos);
    REQUIRE(create.sql.substr(0, clause).find("REFERENCES") == std::string::npos);

    std::size_t references_count = 0;
    for(auto offset = create.sql.find("REFERENCES"); offset != std::string::npos;
        offset = create.sql.find("REFERENCES", offset + 1))
    {
        ++references_count;
    }

    REQUIRE(references_count == 1);
}

TEST_CASE("single-column references keep their column-level clause", "[relations][ddl]")
{
    const auto model = entityfy::describe_model<cr_single>(entityfy::dialect::sqlite);
    const auto create = entityfy::create_table(model);
    INFO(create.sql);

    REQUIRE(
        create.sql.find(
            "\"parent_code\" TEXT NOT NULL REFERENCES \"cr_parents\" (\"code\") ON DELETE RESTRICT ON UPDATE RESTRICT"
        ) != std::string::npos
    );
    REQUIRE(create.sql.find("FOREIGN KEY") == std::string::npos);
}

TEST_CASE("table-level foreign keys map RESTRICT to NO ACTION for T-SQL", "[relations][composite][ddl]")
{
    const auto model = entityfy::describe_model<cr_guarded>(entityfy::dialect::sqlserver);
    const auto create = entityfy::create_table(model);
    INFO(create.sql);

    REQUIRE(
        create.sql.find(
            "FOREIGN KEY (\"tenant_id\", \"parent_code\") REFERENCES \"cr_parents\" (\"tenant_id\", \"code\") "
            "ON DELETE NO ACTION ON UPDATE NO ACTION"
        ) != std::string::npos
    );
}

TEST_CASE("a composite group with one column is rejected", "[relations][composite]")
{
    const auto message = rejection_message([] {
        (void)entityfy::describe_model<cr_bad_group>(entityfy::dialect::sqlite);
    });

    INFO(message);
    REQUIRE(message.find("group 4") != std::string::npos);
    REQUIRE(message.find("cr_bad_group") != std::string::npos);
    REQUIRE(message.find("one column") != std::string::npos);
}

TEST_CASE("a composite group with conflicting actions is rejected", "[relations][composite]")
{
    const auto message = rejection_message([] {
        (void)entityfy::describe_model<cr_mixed_actions>(entityfy::dialect::sqlite);
    });

    INFO(message);
    REQUIRE(message.find("group 5") != std::string::npos);
    REQUIRE(message.find("ON DELETE/ON UPDATE") != std::string::npos);
}

TEST_CASE("sqlite enforces a composite foreign key and cascades the whole key", "[relations][composite][sqlite]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<cr_parent>();
    db.migrate<cr_child>();

    REQUIRE(db.insert(cr_parent{.tenant_id = 1, .code = "eu", .name = "europe"}).rows_affected == 1);
    REQUIRE(db.insert(cr_parent{.tenant_id = 1, .code = "us", .name = "americas"}).rows_affected == 1);

    REQUIRE(db.insert(cr_child{.id = 10, .tenant_id = 1, .parent_code = "eu", .name = "first"}).rows_affected == 1);
    REQUIRE(db.insert(cr_child{.id = 11, .tenant_id = 1, .parent_code = "us", .name = "second"}).rows_affected == 1);

    // One matching key part is not a parent: both halves have to match.
    REQUIRE_THROWS_AS(
        db.insert(cr_child{.id = 12, .tenant_id = 1, .parent_code = "apac", .name = "orphan"}),
        entityfy::foreign_key_violation
    );
    REQUIRE_THROWS_AS(
        db.insert(cr_child{.id = 13, .tenant_id = 2, .parent_code = "eu", .name = "orphan"}),
        entityfy::foreign_key_violation
    );

    REQUIRE(db.count<cr_child>() == 2);

    // ON DELETE CASCADE follows the whole key: only the eu child goes away.
    REQUIRE(db.remove_by_key<cr_parent>(parent_key(1, "eu")) == 1);
    REQUIRE(db.count<cr_parent>() == 1);
    REQUIRE(db.count<cr_child>() == 1);
    REQUIRE(db.find<cr_child>(std::int64_t{11}).has_value());
    REQUIRE_FALSE(db.find<cr_child>(std::int64_t{10}).has_value());
}

TEST_CASE("sqlite introspection folds a composite key into one foreign key", "[relations][composite][sqlite][introspection]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<cr_parent>();
    db.migrate<cr_child>();

    // `PRAGMA foreign_key_list` emits one row per column; both rows belong to
    // one constraint and must not become two independent keys.
    const auto table = db.inspect_table("cr_children");
    INFO("foreign_keys=" << table.foreign_keys.size());
    REQUIRE(table.foreign_keys.size() == 1);

    const auto& foreign_key = table.foreign_keys.front();
    REQUIRE(foreign_key.referenced_table == "cr_parents");
    REQUIRE(foreign_key.columns == std::vector<std::string>{"tenant_id", "parent_code"});
    REQUIRE(foreign_key.referenced_columns == std::vector<std::string>{"tenant_id", "code"});

    // The singular fields repeat the first pair of the lists.
    REQUIRE(foreign_key.column == "tenant_id");
    REQUIRE(foreign_key.referenced_column == "tenant_id");
    REQUIRE(foreign_key.on_delete == "CASCADE");
    REQUIRE(foreign_key.on_update == "CASCADE");

    const auto validation = entityfy::validate_schema(
        entityfy::describe_model<cr_child>(entityfy::dialect::sqlite),
        table
    );
    INFO(entityfy::format_schema_issues(validation));
    REQUIRE(validation.valid());
}

TEST_CASE("sqlite introspection keeps a single-column foreign key as one entry", "[relations][sqlite][introspection]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<cr_parent>();
    db.migrate<cr_single>();

    const auto table = db.inspect_table("cr_singles");
    INFO("foreign_keys=" << table.foreign_keys.size());
    REQUIRE(table.foreign_keys.size() == 1);

    const auto& foreign_key = table.foreign_keys.front();
    REQUIRE(foreign_key.referenced_table == "cr_parents");
    REQUIRE(foreign_key.columns == std::vector<std::string>{"parent_code"});
    REQUIRE(foreign_key.referenced_columns == std::vector<std::string>{"code"});
    REQUIRE(foreign_key.column == "parent_code");
    REQUIRE(foreign_key.referenced_column == "code");
    REQUIRE(foreign_key.on_delete == "RESTRICT");
    REQUIRE(foreign_key.on_update == "RESTRICT");

    const auto validation = entityfy::validate_schema(
        entityfy::describe_model<cr_single>(entityfy::dialect::sqlite),
        table
    );
    INFO(entityfy::format_schema_issues(validation));
    REQUIRE(validation.valid());
}

TEST_CASE("sqlite composite relationship helpers match every key part", "[relations][composite][sqlite]")
{
    entityfy::client db{"sqlite://:memory:"};
    db.migrate<cr_parent>();
    db.migrate<cr_child>();

    require_composite_relationship_navigation(db);
}

#ifdef ENTITYFY_ENABLE_POSTGRESQL

TEST_CASE("postgres enforces a composite foreign key and cascades the whole key", "[relations][composite][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};
    drop_composite_reference_tables(db);

    db.migrate<cr_parent>();

    // Validation after the migration compares the whole composite key: the
    // introspection reports one foreign key with both column lists, so the
    // created table satisfies the model.
    db.migrate<cr_child>();

    REQUIRE(db.insert(cr_parent{.tenant_id = 1, .code = "eu", .name = "europe"}).rows_affected == 1);
    REQUIRE(db.insert(cr_parent{.tenant_id = 1, .code = "us", .name = "americas"}).rows_affected == 1);

    REQUIRE(db.insert(cr_child{.id = 10, .tenant_id = 1, .parent_code = "eu", .name = "first"}).rows_affected == 1);
    REQUIRE(db.insert(cr_child{.id = 11, .tenant_id = 1, .parent_code = "us", .name = "second"}).rows_affected == 1);

    REQUIRE_THROWS_AS(
        db.insert(cr_child{.id = 12, .tenant_id = 1, .parent_code = "apac", .name = "orphan"}),
        entityfy::foreign_key_violation
    );
    REQUIRE_THROWS_AS(
        db.insert(cr_child{.id = 13, .tenant_id = 2, .parent_code = "eu", .name = "orphan"}),
        entityfy::foreign_key_violation
    );

    REQUIRE(db.count<cr_child>() == 2);

    REQUIRE(db.remove_by_key<cr_parent>(parent_key(1, "eu")) == 1);
    REQUIRE(db.count<cr_parent>() == 1);
    REQUIRE(db.count<cr_child>() == 1);
    REQUIRE(db.find<cr_child>(std::int64_t{11}).has_value());
    REQUIRE_FALSE(db.find<cr_child>(std::int64_t{10}).has_value());

    // The migration suite inspects this database; leave no table behind.
    drop_composite_reference_tables(db);
}

TEST_CASE("postgres introspection folds a composite key into one foreign key", "[relations][composite][postgresql][introspection]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};
    drop_composite_reference_tables(db);

    db.migrate<cr_parent>();
    db.migrate<cr_child>();
    db.migrate<cr_note>();

    // information_schema.constraint_column_usage has no position, so the old
    // join produced the cartesian product of the key. The catalog pair is
    // ordered and must surface as one key with both lists.
    const auto child_table = db.inspect_table("cr_children");
    INFO("foreign_keys=" << child_table.foreign_keys.size());
    REQUIRE(child_table.foreign_keys.size() == 1);

    const auto& composite = child_table.foreign_keys.front();
    REQUIRE(composite.referenced_table == "cr_parents");
    REQUIRE(composite.columns == std::vector<std::string>{"tenant_id", "parent_code"});
    REQUIRE(composite.referenced_columns == std::vector<std::string>{"tenant_id", "code"});
    REQUIRE(composite.column == "tenant_id");
    REQUIRE(composite.referenced_column == "tenant_id");
    REQUIRE(composite.on_delete == "CASCADE");
    REQUIRE(composite.on_update == "CASCADE");

    const auto child_validation = entityfy::validate_schema(
        entityfy::describe_model<cr_child>(entityfy::dialect::postgresql),
        child_table
    );
    INFO(entityfy::format_schema_issues(child_validation));
    REQUIRE(child_validation.valid());

    const auto note_table = db.inspect_table("cr_notes");
    INFO("foreign_keys=" << note_table.foreign_keys.size());
    REQUIRE(note_table.foreign_keys.size() == 1);

    const auto& single = note_table.foreign_keys.front();
    REQUIRE(single.referenced_table == "cr_children");
    REQUIRE(single.columns == std::vector<std::string>{"child_id"});
    REQUIRE(single.referenced_columns == std::vector<std::string>{"id"});
    REQUIRE(single.column == "child_id");
    REQUIRE(single.referenced_column == "id");
    REQUIRE(single.on_delete == "CASCADE");
    REQUIRE(single.on_update == "CASCADE");

    const auto note_validation = entityfy::validate_schema(
        entityfy::describe_model<cr_note>(entityfy::dialect::postgresql),
        note_table
    );
    INFO(entityfy::format_schema_issues(note_validation));
    REQUIRE(note_validation.valid());

    drop_composite_reference_tables(db);
}

TEST_CASE("postgres composite relationship helpers match every key part", "[relations][composite][postgresql]")
{
    const auto uri = postgres_uri();
    if(uri.empty())
    {
        SKIP("ENTITYFY_PG_URI is not set");
    }

    entityfy::client db{uri};
    drop_composite_reference_tables(db);

    db.migrate<cr_parent>();
    db.migrate<cr_child>();

    require_composite_relationship_navigation(db);

    // The migration suite inspects this database; leave no table behind.
    drop_composite_reference_tables(db);
}

#endif // ENTITYFY_ENABLE_POSTGRESQL
