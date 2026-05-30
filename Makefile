# nl-router top-level Makefile.
# Conveniences for development; CI builds use the same recipes under the hood.

# ---- Configuration --------------------------------------------------------

# Default DSN points at the docker-compose Postgres. Override via env for
# remote/test databases:  make migrate DATABASE_URL=postgres://...
DATABASE_URL ?= postgres://nl_router:nl_router@localhost:5432/nl_router?sslmode=disable
MIGRATE       ?= migrate
MIGRATIONS_DIR := migrations

# ---- Phony targets --------------------------------------------------------

.PHONY: help migrate migrate-up migrate-down migrate-status migrate-new \
        db-up db-down db-reset \
        dev dev-up dev-down \
        monitoring-up monitoring-down monitoring-logs \
        psql clean

help:
	@echo "Common targets:"
	@echo "  make db-up           Start Postgres + PgBouncer via docker-compose"
	@echo "  make db-down         Stop the docker-compose stack"
	@echo "  make db-reset        Drop + recreate the database (destroys all data)"
	@echo "  make migrate         Apply all up migrations"
	@echo "  make migrate-down N=1  Roll back the last N migrations"
	@echo "  make migrate-status  Show current schema version"
	@echo "  make migrate-new NAME=desc  Create a new migration pair"
	@echo "  make psql            Open a psql shell against the dev database"
	@echo "  make dev             Bring up the full dev stack"
	@echo "  make monitoring-up   Start Prometheus + Grafana (see monitoring/README.md)"
	@echo "  make monitoring-down Stop the monitoring stack (keeps volumes)"
	@echo "  make monitoring-logs Tail Prometheus + Grafana logs"

# ---- Migrations -----------------------------------------------------------

# Apply all pending up migrations.
migrate: migrate-up

migrate-up:
	$(MIGRATE) -path $(MIGRATIONS_DIR) -database "$(DATABASE_URL)" up

# Roll back N migrations (default: 1).
migrate-down:
	$(MIGRATE) -path $(MIGRATIONS_DIR) -database "$(DATABASE_URL)" down $(if $(N),$(N),1)

migrate-status:
	$(MIGRATE) -path $(MIGRATIONS_DIR) -database "$(DATABASE_URL)" version

# Scaffold a new migration pair: NAME=<short_description>
# Produces NNNN_short_description.{up,down}.sql with sequential numbering.
migrate-new:
	@if [ -z "$(NAME)" ]; then echo "Usage: make migrate-new NAME=short_description" >&2; exit 1; fi
	$(MIGRATE) create -ext sql -dir $(MIGRATIONS_DIR) -seq $(NAME)

# ---- Database lifecycle (docker-compose) ----------------------------------

db-up:
	docker compose up -d postgres pgbouncer

db-down:
	docker compose down

db-reset: db-down
	docker volume rm -f nl-router_pgdata 2>/dev/null || true
	$(MAKE) db-up
	@echo "Waiting for Postgres to accept connections..."
	@until docker compose exec -T postgres pg_isready -U nl_router -d nl_router >/dev/null 2>&1; do sleep 1; done
	$(MAKE) migrate

# ---- Full dev stack -------------------------------------------------------

dev: dev-up

dev-up:
	docker compose up -d

dev-down:
	docker compose down

# ---- Monitoring (Prometheus + Grafana) -----------------------------------
# See monitoring/README.md for details. The stack scrapes localhost on
# the nl-router metric ports (9180-9184) so it runs colocated with the
# .deb-installed daemons.

monitoring-up:
	cd monitoring && docker compose up -d

monitoring-down:
	cd monitoring && docker compose down

monitoring-logs:
	cd monitoring && docker compose logs -f --tail=50

# ---- Utilities ------------------------------------------------------------

psql:
	docker compose exec postgres psql -U nl_router -d nl_router

clean:
	@echo "Nothing to clean yet (no build artifacts)."
