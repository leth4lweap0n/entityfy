# entityfy-migrate

An Entity Framework-style migration engine: snapshot → diff → migration file → apply/rollback,
plus runnable SQL script generation and model header generation from the live schema (scaffold).

## Command-line tool

```bash
./build/release/bin/entityfy_migrate <command> [options]
```

| Command | Purpose |
|---|---|
| `snapshot` | Produces `schema.snapshot.json` from the models |
| `add <name>` | Writes the difference between the snapshot and the live schema to a migration file |
| `plan` | Shows the SQL of pending migrations (does not apply) |
| `apply` | Applies pending migrations inside a transaction |
| `status` | Applied/pending migrations and schema drift |
| `script` | Generates runnable SQL for pending migrations |
| `scaffold` | Generates a C++ model header from the live schema |
| `baseline` | Marks the existing database as migrated without applying migrations (`--force`) |
| `rollback` | Reverts the last applied migration |
| `rename-table` | Renames a table in the database in place (`--to` gives the target) |

Options: `--db <uri>`, `--provider sqlite|postgres|mysql|sqlserver`, `--root <directory>`,
`--rename table.column=new`, `--allow-destructive`, `--no-backup`, `--dry-run`,
`--output <file>`, `--force`.

Example:

```bash
entityfy_migrate snapshot --db sqlite://app.db --root migrations/sqlite
entityfy_migrate add "add nickname" --db sqlite://app.db --root migrations/sqlite
entityfy_migrate plan  --db sqlite://app.db --root migrations/sqlite
entityfy_migrate apply --db sqlite://app.db --root migrations/sqlite
entityfy_migrate script --db sqlite://app.db --root migrations/sqlite --output migrate.sql
```

## Library API

```cpp
entityfy::migrate::engine runner{
    *provider,
    entityfy::migrate::engine_options{
        .root = "migrations/sqlite",
        .provider_name = "sqlite",
        .allow_destructive = false,
        .backup = true,
    }};

runner.add(schema, "add nickname");   // writes a migration file
runner.plan();                        // pending migrations
runner.apply();                       // applies
runner.status();                      // status + drift
runner.rollback();                    // reverts the last migration

std::ostringstream script;
runner.script_all(script);            // runnable SQL for all migrations
```

### File format

Each migration is a `migrations/<provider>/<timestamp>_<id>.json` file:

```json
{
  "id": "20261021195557_001_init",
  "provider": "sqlite",
  "checksum": "…",
  "statements": [{"sql": "CREATE TABLE …"}]
}
```

`schema.snapshot.json` holds the expected schema; the `add` command diffs the snapshot against
the live schema.

## History table

Applied migrations are kept in the `entityfy_schema_migrations` table
(`id`, `checksum`, `seq`, `applied_at`). Records are read from and written to this table only.

### Upgrade (databases from older versions)

In this version the history table name changed. If the table still exists under the old name in
an existing database, move it to the new name while preserving the records:

```bash
entityfy_migrate rename-table <old_table_name> --to entityfy_schema_migrations --db <uri>
```

The command runs `ALTER TABLE ... RENAME TO ...`; table names are escaped with `quote_ident`, so
names containing `"` are handled safely too. No old name is hard-coded in the command; different
users can move their own tables with this command. Before migrating, run `entityfy_migrate status`
and check the pending migrations.

## Rebuild rules

- For tables that share a column, rebuild copies the data.
- For tables with **no shared column at all**, rebuild is rejected due to the risk of row loss;
  if the table does not exist in the source, "0 rows" is assumed, so a script can be generated
  from an empty source.
- Because trigger/sequence information is not kept in the snapshot, `--all` reads them from the
  source DB.

## Safety

- By default destructive operations (drop, type change) are rejected; they are enabled with
  `--allow-destructive`.
- A backup is taken before a destructive operation (disabled with `--no-backup`). SQLite uses
  `VACUUM INTO` (`.db`), PostgreSQL needs none because its DDL is transactional, and the MySQL and
  SQL Server providers write a SQL dump (`.sql`) with `CREATE TABLE`/`INSERT` statements that can be
  replayed on a clean database.
- Before applying, the expected schema is compared with the database; a mismatch raises an error.
- A migration file is protected by checksum; if the file is changed afterwards, applying stops.

## Tests

`entityfy_migrate_tests` runs the following suites: `schema`, `diff`, `json`, `sqlite`,
`engine`, `scaffold`, `history_table`, `rename_table`, `owned_model`, and `postgres`
(enabled with `ENTITYFY_PG_URI`).
