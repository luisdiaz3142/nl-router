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
| Python rule CRUD integration | `python/tests/test_integration_rules.py` | create / list / get / update / delete / auth / audit, all against a real Postgres |

60+ Python tests, 37 C++ tests. The integration tests in
`test_integration_rules.py` exercise the full FastAPI route →
Pydantic → SQL → audit chain and auto-skip when no Postgres is
reachable, so they don't gate the unit tests on a fresh checkout.

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

### Running integration tests locally

`test_integration_rules.py` (and any future `test_integration_*.py`
files) exercise the route handlers against a real Postgres. They
auto-skip when no DB is reachable, so the unit-test suite stays
green on a fresh checkout. To run them locally:

```sh
make db-up        # docker-compose Postgres
make migrate      # apply schema
make test-py      # integration tests now run (look for test_integration_*)
```

If your dev DB is on a non-default DSN (custom port, hosted Postgres),
override via env:

```sh
NL_ROUTER_TEST_DSN=postgres://user:pass@host:5433/db make test-py
```

`make test-py PYTEST_ARGS="python/tests/test_integration_rules.py"` to
run just the integration set. The fixtures TRUNCATE the mutable
tables before each test, so order doesn't matter and a crashed test
doesn't leak state — but seed tables (`processing_modules`,
`system_config`) are preserved since migrations populate them.

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

## Deploying to a remote host

`make redeploy HOST=<hostname>` is the one-command path that
collapses build → scp → install → restart into a single laptop-side
invocation. Logic lives in
[`packaging/scripts/redeploy.sh`](../packaging/scripts/redeploy.sh)
(orchestrator on your laptop) and
[`packaging/scripts/remote-install.sh`](../packaging/scripts/remote-install.sh)
(runs on the remote host).

```sh
make redeploy HOST=dicom-diablo
```

The script prefers a CI-built `.deb` (downloaded via `gh run
download`); falls back to a local Docker build if `gh` isn't
installed or no green workflow run exists on HEAD. After scp'ing
the `.deb` and `remote-install.sh` to the host, the remote script:

1. `apt install --reinstall` the new `.deb`
2. `nl-router init` — regenerates env files from `config.toml`
3. `systemctl daemon-reload` — picks up any unit-file changes
4. `systemctl reset-failed` — clears crash-restart counters
5. Restarts every enabled core daemon and module-worker instance
6. Final `is-active` health check; non-zero exit if any service
   isn't running after restart

Idempotent — re-running produces the same end state.

### SSH through gcloud IAP / bastion hosts

If plain `ssh <host>` doesn't reach your VM (typical for GCP
instances behind IAP), wrap the underlying commands with the
`NLR_SSH` and `NLR_SCP` env vars.

`NLR_SSH` is treated as a self-contained "ssh-to-the-right-host"
prefix — the script doesn't append `$HOST` to it. `NLR_SCP` is
used as-is and the script supplies `host:path` arguments to it.

For gcloud IAP:

```sh
NLR_SSH='gcloud compute ssh dicom-diablo --tunnel-through-iap --' \
NLR_SCP='gcloud compute scp --tunnel-through-iap' \
    make redeploy HOST=dicom-diablo
```

Notes:

- The trailing `--` in NLR_SSH is required — it tells gcloud that
  the next arg is the remote command, not a gcloud flag.
- `HOST=` is still passed because SCP uses `host:path` syntax;
  it's the destination, not a flag.

To save yourself from re-typing every time, add to your shell rc:

```sh
export NLR_SSH='gcloud compute ssh dicom-diablo --tunnel-through-iap --'
export NLR_SCP='gcloud compute scp --tunnel-through-iap'
```

Then `make redeploy HOST=dicom-diablo` works directly.

### Caveats

- **Services restart cold.** Mid-flight API requests and DICOM
  associations fail. Fine for dev / single-node pilot; production
  rolling-restart is out of scope here.
- **Single host per invocation.** Use a shell loop for multiple
  hosts; Ansible / pyinfra starts making sense at 3+ hosts.
- **No automatic rollback.** If `apt install` fails the host stays
  at the previous version; if the install succeeds but services
  crash, you'll get a non-zero exit but the package is already
  swapped in. Manual `apt install <previous-version.deb>` is the
  rollback path.

## CI

Every push to `main` and every pull request triggers
[`.github/workflows/build.yml`](../.github/workflows/build.yml) on
GitHub Actions:

| Job | What it does | Typical runtime |
|---|---|---|
| `test` | C++ build + ctest + Python pytest. Uses Debian/Ubuntu apt DCMTK (not the static build from packaging) — faster, same API surface. | ~5 min |
| `package` | Docker-built `.deb` via `packaging/Dockerfile.build`, then `packaging/scripts/check-deb-contents.sh` to verify every required file actually shipped. Uploads the `.deb` as a workflow artifact (14-day retention). | ~15 min cold, ~5 min warm |

Both jobs run in parallel. CI failure does not auto-block merge —
configure a branch protection rule in repo settings if you want
that enforced.

### Grabbing the .deb from a green CI run

For any successful workflow run on `main`, the produced `.deb`
is downloadable from the run page:

1. GitHub repo → **Actions** → most recent green `build` run
2. Scroll to **Artifacts** at the bottom
3. Download **`nl-router-deb-amd64`**
4. Unzip, then `sudo apt install ./nl-router_0.0.1_amd64.deb`

This is the same artifact you'd build locally via
`ARCHES=amd64 packaging/build-all.sh` — same build container,
same nfpm manifest, same smoke-test gate.

### Packaging smoke test

`packaging/scripts/check-deb-contents.sh` asserts that every
operator-critical path is present in a built `.deb`. Add a path
when a new artifact is shipped (e.g. a new module binary, a new
systemd unit). Run locally against a freshly-built `.deb`:

```sh
packaging/scripts/check-deb-contents.sh packaging/dist/nl-router_*.deb
```

Catches the bug class where a file is built and staged but not
listed in `packaging/nfpm/nfpm.yaml` — the silent dropout that
shipped from M22 to M27.

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
