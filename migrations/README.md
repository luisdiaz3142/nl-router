# Migrations

SQL migrations for nl-router, applied with [golang-migrate](https://github.com/golang-migrate/migrate).

## Layout

Each migration is a pair: `NNNN_description.up.sql` and `NNNN_description.down.sql`.
Sequential numbering. Up applies the change; down reverts it.

## Conventions

- Every migration is wrapped in a `BEGIN; ... COMMIT;` block.
- Down migrations must cleanly reverse the corresponding up. Test both
  directions on a populated DB before merging.
- New columns: prefer nullable adds; backfill in a follow-up migration; only
  then add `NOT NULL`. Avoids long table rewrites under load.
- New indexes on large tables: use `CREATE INDEX CONCURRENTLY` (incompatible
  with the surrounding transaction — use a separate non-transactional
  migration when this matters).
- Avoid `ALTER TYPE ... ADD VALUE` inside a transaction (Postgres rejects it).

## Current schema (after 0008)

| File | What it adds |
|---|---|
| `0001_init_enums` | `work_status`, `close_trigger`, `rule_status`, `rule_scope`, `job_status`, `user_role` enums |
| `0002_work_queue` | `work_queue` (the central pipeline table; 50+ scalar columns + JSONB) and `route_assignments` |
| `0003_rules_and_destinations` | `rules`, `destinations`, `rule_destinations`, `processing_modules`, `rule_processing_chain`; seeds the 7 built-in modules; back-fills FKs on `route_assignments` |
| `0004_processing_jobs` | `processing_jobs` queue table for module workers |
| `0005_credentials` | `credentials` table; back-fills FK on `destinations.credential_id` |
| `0006_system_config` | `system_config` table with seed defaults for receiver / router / dispatcher / cleaner / retention / credentials / api |
| `0007_auth` | `users`, `api_tokens`, `login_attempts` |
| `0008_admin_audit` | `admin_audit` append-only log |

## Running

```bash
# Apply all pending migrations
make migrate

# Or directly:
migrate -path migrations \
        -database "postgres://nl_router:nl_router@localhost:5432/nl_router?sslmode=disable" \
        up

# Roll back the last migration
make migrate-down

# Show the current version
make migrate-status

# Create a new migration pair
make migrate-new NAME=add_my_thing
```
