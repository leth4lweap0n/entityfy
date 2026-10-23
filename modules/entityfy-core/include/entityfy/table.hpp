#pragma once

#include <entityfy/detail/backend.hpp>
#include <entityfy/mapper.hpp>
#include <entityfy/migration.hpp>
#include <entityfy/query.hpp>
#include <entityfy/schema_validation.hpp>
#include <entityfy/statement.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace entityfy
{

struct insert_result
{
    std::uint64_t rows_affected = 0;
    std::int64_t  last_insert_id = 0;
};

struct schema_sync_options
{
    bool                      validate_after = true;
    bool                      force = false;
    schema_validation_options validation{};
};

template <typename Model>
class table_client
{
public:
    explicit table_client(detail::backend& backend)
        : backend_{&backend},
          model_{describe_model<Model>(backend.target_dialect())}
    {
        detail::validate_model<Model>();
    }

    [[nodiscard]] dialect target_dialect() const noexcept
    {
        return model_.target;
    }

    [[nodiscard]] const model_descriptor<Model>& descriptor() const noexcept
    {
        return model_;
    }

    void migrate()
    {
        migrate(schema_sync_options{});
    }

    void migrate(schema_sync_options options)
    {
        const auto plan = plan_migration(model_, backend_->table_columns(model_.table_name));

        if(options.force)
        {
            const auto current_schema = backend_->inspect_table(model_.table_name);
            if(!current_schema.exists())
            {
                apply_schema_statements(create_schema_statements(model_));
            }
            else
            {
                const auto validation = entityfy::validate_schema(model_, current_schema, options.validation);
                if(!validation.valid())
                {
                    reset_schema();
                }
            }
        }
        else
        {
            for(const auto& ddl: plan.statements)
            {
                backend_->execute(ddl);
            }
        }

        if(options.validate_after)
        {
            require_schema(options.validation);
        }
    }

    void migrate_force(schema_validation_options validation = {})
    {
        migrate(schema_sync_options{
            .validate_after = true,
            .force = true,
            .validation = std::move(validation),
        });
    }

    void reset_schema()
    {
        backend_->execute(drop_table(model_));
        apply_schema_statements(create_schema_statements(model_));
    }

    void migrate_versioned(std::string_view id)
    {
        const auto plan = plan_migration(model_, backend_->table_columns(model_.table_name));
        apply_migration(*backend_, migration{
            .id = std::string{id},
            .statements = plan.statements,
        });
        require_schema();
    }

    void migrate_versioned()
    {
        migrate_versioned(model_.table_name + "_schema");
    }

    void sync_schema()
    {
        migrate();
    }

    [[nodiscard]] schema_validation_result validate_schema(schema_validation_options options = {})
    {
        return entityfy::validate_schema(model_, backend_->inspect_table(model_.table_name), std::move(options));
    }

    void require_schema(schema_validation_options options = {})
    {
        entityfy::require_schema(model_, backend_->inspect_table(model_.table_name), std::move(options));
    }

    [[nodiscard]] insert_result insert(const Model& model)
    {
        const auto result = backend_->execute(insert_statement(model, model_));
        return insert_result{
            .rows_affected = result.rows_affected,
            .last_insert_id = result.last_insert_id,
        };
    }

    [[nodiscard]] insert_result create(const Model& model)
    {
        return insert(model);
    }

    [[nodiscard]] std::uint64_t update(const Model& model)
    {
        return backend_->execute(update_statement(model, model_)).rows_affected;
    }

    // Writes only the given columns. Use it when the caller already knows what
    // changed and does not want to rewrite (or compare) the whole row.
    template <typename... Members>
    [[nodiscard]] std::uint64_t update_partial(const Model& model, Members Model::*... members)
    {
        return backend_->execute(update_partial_statement(model, model_, members...)).rows_affected;
    }

    [[nodiscard]] std::vector<Model> find_many(condition filter = {})
    {
        return collect(select_statement(model_, std::move(filter)));
    }

    [[nodiscard]] std::vector<Model> find_many(query_options<Model> options)
    {
        return collect(select_statement(model_, std::move(options)));
    }

    [[nodiscard]] std::vector<Model> find_many(const query_builder<Model>& query)
    {
        return find_many(query.options());
    }

    [[nodiscard]] std::optional<Model> find_one(condition filter = {})
    {
        return collect_one(select_one_statement(model_, std::move(filter)));
    }

    [[nodiscard]] std::optional<Model> find_one(query_options<Model> options)
    {
        return collect_one(select_one_statement(model_, std::move(options)));
    }

    [[nodiscard]] std::optional<Model> find_one(const query_builder<Model>& query)
    {
        return find_one(query.options());
    }

    [[nodiscard]] std::uint64_t count(condition filter = {})
    {
        const auto rows = backend_->query(count_statement(model_, std::move(filter)));
        if(rows.empty() || rows.front().empty())
        {
            return 0;
        }

        return detail::value_to_u64(rows.front().front());
    }

    [[nodiscard]] std::uint64_t count(query_options<Model> options)
    {
        return count(std::move(options.filter));
    }

    [[nodiscard]] std::uint64_t count(const query_builder<Model>& query)
    {
        return count(query.options());
    }

    [[nodiscard]] bool exists(condition filter = {})
    {
        bool found = false;
        backend_->query_each(exists_statement(model_, std::move(filter)), [&](const detail::row&) {
            found = true;
            return false;
        });

        return found;
    }

    [[nodiscard]] bool exists(query_options<Model> options)
    {
        return exists(std::move(options.filter));
    }

    [[nodiscard]] bool exists(const query_builder<Model>& query)
    {
        return exists(query.options());
    }

    [[nodiscard]] insert_result insert_many(const std::vector<Model>& models)
    {
        insert_result summary;

        if(models.empty())
        {
            return summary;
        }

        // Auto-increment models must read generated ids row by row,
        // so the multi-row path cannot be used here.
        if(has_generated_primary_key(model_))
        {
            for(const auto& model: models)
            {
                const auto current = insert(model);
                summary.rows_affected += current.rows_affected;
                summary.last_insert_id = current.last_insert_id;
            }

            return summary;
        }

        const auto columns = insertable_columns(model_);
        const std::size_t column_count = std::max<std::size_t>(1, columns.size());
        const std::size_t limit = backend_->batch_parameter_limit();
        const std::size_t max_rows = std::max<std::size_t>(1, limit / column_count);

        std::vector<entityfy::statement> commands;
        commands.reserve((models.size() + max_rows - 1) / max_rows);

        for(std::size_t chunk = 0; chunk < models.size(); chunk += max_rows)
        {
            const std::size_t chunk_end = std::min(models.size(), chunk + max_rows);
            const std::vector<Model> batch{models.begin() + static_cast<std::ptrdiff_t>(chunk),
                                           models.begin() + static_cast<std::ptrdiff_t>(chunk_end)};

            commands.push_back(insert_many_statement(batch, model_));
        }

        for(const auto& executed: backend_->execute_batch(commands))
        {
            summary.rows_affected += executed.rows_affected;
        }

        return summary;
    }

    [[nodiscard]] insert_result create_many(const std::vector<Model>& models)
    {
        return insert_many(models);
    }

    [[nodiscard]] insert_result upsert(const Model& model)
    {
        const auto result = backend_->execute(upsert_statement(model, model_));
        return insert_result{
            .rows_affected = result.rows_affected,
            .last_insert_id = result.last_insert_id,
        };
    }

    [[nodiscard]] std::uint64_t update_many(const Model& patch, condition filter)
    {
        return backend_->execute(update_many_statement(patch, model_, std::move(filter))).rows_affected;
    }

    [[nodiscard]] std::uint64_t update_many(const Model& patch, query_options<Model> options)
    {
        return update_many(patch, std::move(options.filter));
    }

    [[nodiscard]] std::uint64_t update_many(const Model& patch, const query_builder<Model>& query)
    {
        return update_many(patch, query.options());
    }

    [[nodiscard]] std::uint64_t delete_many(condition filter)
    {
        return remove(std::move(filter));
    }

    [[nodiscard]] std::uint64_t delete_many(query_options<Model> options)
    {
        return remove(std::move(options.filter));
    }

    [[nodiscard]] std::uint64_t delete_many(const query_builder<Model>& query)
    {
        return delete_many(query.options());
    }

    [[nodiscard]] std::uint64_t delete_all()
    {
        return backend_->execute(delete_statement(model_, condition{})).rows_affected;
    }

    template <typename Related, typename ForeignKey>
    [[nodiscard]] std::vector<Related> has_many(const Model& parent, ForeignKey Related::* foreign_key)
    {
        require_single_column_key("has_many");
        const auto key = detail::primary_key_value(parent, model_);
        return table_client<Related>{*backend_}.find_many(where(foreign_key).eq(key));
    }

    template <typename Related, typename ForeignKey>
    [[nodiscard]] std::optional<Related> has_one(const Model& parent, ForeignKey Related::* foreign_key)
    {
        require_single_column_key("has_one");
        const auto key = detail::primary_key_value(parent, model_);
        return table_client<Related>{*backend_}.find_one(where(foreign_key).eq(key));
    }

    template <typename Parent, typename ForeignKey>
    [[nodiscard]] std::optional<Parent> belongs_to(const Model& child, ForeignKey Model::* foreign_key)
    {
        table_client<Parent> parents{*backend_};

        if(primary_key_columns(parents.descriptor()).size() != 1)
        {
            throw std::logic_error{
                "entityfy ORM belongs_to requires the parent model to have a single-column primary key; composite foreign "
                "keys are not supported yet"
            };
        }

        return parents.find(child.*foreign_key);
    }

private:
    // Composite foreign keys are phase P1.5; until they land the navigation
    // helpers reject a composite parent key instead of guessing a column.
    void require_single_column_key(std::string_view operation) const
    {
        if(primary_key_columns(model_).size() != 1)
        {
            throw std::logic_error{
                "entityfy ORM " + std::string{operation} +
                " requires a single-column primary key; composite foreign keys are not supported yet"
            };
        }
    }

    void apply_schema_statements(const std::vector<statement>& statements)
    {
        for(const auto& ddl: statements)
        {
            backend_->execute(ddl);
        }
    }

    // Streams the statement and materializes every row in place: no outer
    // vector of rows and no per-row copy of sql_value vectors.
    [[nodiscard]] std::vector<Model> collect(const entityfy::statement& command)
    {
        std::vector<Model> output;
        backend_->query_each(command, [&](const detail::row& current) {
            output.emplace_back(detail::materialize_row<Model>(current));
            return true;
        });

        return output;
    }

    [[nodiscard]] std::optional<Model> collect_one(const entityfy::statement& command)
    {
        std::optional<Model> output;
        backend_->query_each(command, [&](const detail::row& current) {
            output.emplace(detail::materialize_row<Model>(current));
            return false;
        });

        return output;
    }

    [[nodiscard]] static std::vector<Model> materialize_many(const std::vector<detail::row>& rows)
    {
        std::vector<Model> output;
        output.reserve(rows.size());

        for(const auto& row: rows)
        {
            output.emplace_back(detail::materialize_row<Model>(row));
        }

        return output;
    }

public:
    // General key API: composite and non-integral keys go through row_key.
    [[nodiscard]] std::optional<Model> find(const row_key& key)
    {
        return collect_one(select_by_key_statement(model_, key));
    }

    [[nodiscard]] std::uint64_t remove_by_key(const row_key& key)
    {
        return backend_->execute(delete_by_key_statement(model_, key)).rows_affected;
    }

    // Single-value convenience for one-column keys (int64, string, ...). It
    // builds a one-part row_key internally.
    template <typename Id>
        requires(!std::same_as<std::remove_cvref_t<Id>, row_key>)
    [[nodiscard]] std::optional<Model> find(Id&& id)
    {
        return collect_one(select_by_id_statement(model_, std::forward<Id>(id)));
    }

    template <typename Id>
        requires(!std::same_as<std::remove_cvref_t<Id>, row_key>)
    [[nodiscard]] std::optional<Model> find_unique(Id&& id)
    {
        return find(std::forward<Id>(id));
    }

    [[nodiscard]] std::uint64_t remove(condition filter)
    {
        if(filter.empty())
        {
            throw std::invalid_argument{"entityfy::table_client::remove requires a WHERE condition; use execute() for intentional full-table deletes"};
        }

        return backend_->execute(delete_statement(model_, std::move(filter))).rows_affected;
    }

    template <typename Id>
        requires(!std::same_as<std::remove_cvref_t<Id>, row_key>)
    [[nodiscard]] std::uint64_t remove_by_id(Id&& id)
    {
        return backend_->execute(delete_by_id_statement(model_, std::forward<Id>(id))).rows_affected;
    }

private:
    detail::backend*        backend_;
    model_descriptor<Model> model_;
};

} // namespace entityfy
