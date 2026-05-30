# Local development

Quick reference for working on nl-router from a checkout. For end-to-end
test recipes see [`end-to-end-test.md`](end-to-end-test.md); for shipping
artifacts see [`packaging/INSTALL.md`](../packaging/INSTALL.md).

## Setup

```sh
# 1. Postgres + PgBouncer in docker-compose for local dev
make db-up
make migrate                          # apply schema

# 2. Python venv with API + dev extras
python3 -m venv .venv
.venv/bin/pip install -e '.[api,dev]'

# 3. C++ build (one-time configure)
cmake -S cpp -B cpp/build
cmake --build cpp/build -j
```

## Running the test suite

```sh
make test-py                          # python/tests/, ~1 second
ctest --test-dir cpp/build            # C++ tests, also ~1 second
```

### What's covered today

| Layer | Suite | Status |
|---|---|---|
| C++ DSL parser + evaluator | `cpp/common/dsl/tests/` | ~30 cases, full grammar coverage |
| C++ metrics registry | `cpp/common/metrics/tests/` | ~10 cases, exposition + label arity |
| Python crypto | `python/tests/test_crypto.py` | round-trip, tamper, version |
| Python predicate validation | `python/tests/test_models.py` | structural + DSL shellout (M22) |
| Python destination probes | `python/tests/test_probes.py` | shape + dispatch table |
| Python UI helpers | `python/tests/test_common.py` | pill mapping, flash cookie |
| Python SSE encoder | `python/tests/test_routes_sse.py` | framing, newline handling |

40+ Python tests, 37 C++ tests. Anything that touches Postgres is **not**
in this set yet — DB-touching integration tests need an ephemeral test
DB fixture (deferred to a follow-up slice).

### Skipped tests

The DSL-parse tests in `test_models.py` shell out to
`cpp/build/common/dsl/nl-dsl-validate`. If you haven't built the C++ side
yet, those cases auto-skip (you'll see `s` instead of `.`). Build the
helper to re-enable them:

```sh
cmake --build cpp/build --target nl-dsl-validate
```

You can also point at a built copy elsewhere:

```sh
NL_ROUTER_DSL_VALIDATE_BIN=/path/to/nl-dsl-validate make test-py
```

## Filtering / debugging

```sh
# One test file
make test-py PYTEST_ARGS="python/tests/test_crypto.py"

# One test by name (substring match)
make test-py PYTEST_ARGS="-k tamper"

# Verbose + stop on first failure
make test-py PYTEST_ARGS="-vvs --maxfail=1"

# Drop into pdb on failure
make test-py PYTEST_ARGS="--pdb"
```

## Running the management API live

```sh
.venv/bin/uvicorn nl_router.api.app:app --reload --port 8080
```

Pointed at the docker-compose Postgres on `:5432` via the bootstrap
config under `/etc/nl-router/config.toml` — or override with env vars
(`NL_ROUTER_DATABASE_URL`, `NL_ROUTER_SERVER_ID`).

For DSL predicate validation in dev mode, set:

```sh
export NL_ROUTER_DSL_VALIDATE_BIN=$PWD/cpp/build/common/dsl/nl-dsl-validate
```

Otherwise the validator falls through (logs `dsl_validate.skipped
no_binary`), and the router's cache refresh is the only DSL parse check.

## Linting

```sh
.venv/bin/ruff check python/
.venv/bin/ruff format python/         # auto-format
.venv/bin/mypy python/nl_router       # type check
```

Mypy is `strict` per `pyproject.toml`. Ruff config is opinionated but
forgiving (line length handled by formatter, not linter).

## Adding new tests

- Pure unit tests go in `python/tests/test_<module>.py`. Don't open a
  DB pool or HTTP server unless you also add a fixture that owns its
  lifecycle.
- Use the `test_kek_env` fixture (`conftest.py`) for anything that
  encrypts/decrypts — it sets a deterministic test key without
  touching `/etc/nl-router/kek.key`.
- Use the `dsl_validate_bin` fixture for anything that needs the C++
  parser; gate the test body with `if dsl_validate_bin is None:
  pytest.skip(...)`.
- For new features that need a Postgres, **add a TODO and skip the
  test**; the DB fixture is a separate slice.
