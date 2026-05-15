# Installing nl-router

This guide covers building nl-router artifacts from source and installing
them on a Linux host. Until release artifacts are published, every install
path starts with a local build via Docker.

## Prerequisites

| What | Why | Where |
|---|---|---|
| Docker (with `buildx`) | Builds the artifacts in a reproducible Debian-12 container | Docker Desktop / docker-ce 20.10+ |
| Linux host | nl-router is Linux-only (DCMTK + systemd) | bare metal, VM, or container with systemd |
| Postgres 14+ | Central config + work queue | self-hosted, Patroni, RDS, Cloud SQL, etc. |
| Python 3.11+ on the target host | Runs the management CLI + FastAPI under a bundled venv | distro package; postinstall checks the version |

The build itself does not need anything on the host besides Docker. The
target host (where nl-router will run) needs Python 3.11+, libpq5,
libssl3, libdcmtk17 (or your distro's equivalent), and systemd.

## 1. Provision Postgres

If you don't already have one, the dev compose stack starts an empty
Postgres 16 with the right user / database:

```sh
cd /path/to/nl-router
docker compose up -d postgres
```

Note the DSN:

```
postgres://nl_router:nl_router@localhost:5432/nl_router?sslmode=disable
```

For production, use any Postgres 14+ — single instance, Patroni, RDS
Multi-AZ, Cloud SQL HA. nl-router takes a libpq DSN and is otherwise
topology-agnostic.

## 2. Build the artifacts

One command. Produces `.deb`, `.rpm`, and `.tar.gz` for the requested
arches under `packaging/dist/`.

```sh
docker build -t nl-router-build -f packaging/Dockerfile.build packaging/

# Host arch only (fast):
ARCHES=$(uname -m | sed 's/x86_64/amd64/; s/aarch64/arm64/') \
    packaging/build-all.sh

# Or both arches (the foreign one runs under QEMU emulation — slower):
ARCHES="amd64 arm64" packaging/build-all.sh
```

Output:

```
packaging/dist/nl-router_0.0.1_<arch>.deb         Debian / Ubuntu
packaging/dist/nl-router_0.0.1_<arch>.rpm         RHEL / Rocky / Fedora / Amazon Linux
packaging/dist/nl-router-0.0.1-<arch>.tar.gz      Alpine / Arch / generic
```

Add `BUILD_IMAGES=1` to also build a local `nl-router:<version>-<arch>`
container image. Add `PUSH_MANIFEST=ghcr.io/your-org/nl-router` to
assemble a multi-arch manifest on a registry (the per-arch images must
already be `--push`ed; that workflow is CI's job, not this script's).

## 3. Install

Pick one path. All three install the same files in the same paths and
land at the same `nl-router` CLI.

### Debian / Ubuntu

```sh
sudo apt install ./packaging/dist/nl-router_0.0.1_amd64.deb
```

### RHEL / Rocky / Fedora / Amazon Linux

```sh
sudo dnf install ./packaging/dist/nl-router_0.0.1_amd64.rpm
```

### Alpine / Arch / generic Linux

The tarball ships an `install.sh` that reuses the .deb's pre/postinstall
scriptlets. It's POSIX sh — works on BusyBox.

Install the runtime deps your distro provides:

```sh
# Alpine
apk add python3 postgresql-libs openssl libstdc++ libgcc tar shadow

# Arch
sudo pacman -S python postgresql-libs openssl gcc-libs shadow tar

# Amazon Linux 2
sudo yum install python3.11 libpq openssl libstdc++ shadow-utils
```

Then unpack and install:

```sh
tar -xzf packaging/dist/nl-router-0.0.1-amd64.tar.gz
cd nl-router-0.0.1-amd64
sudo ./install.sh
```

The installer:
* creates the `nl-router` system user/group;
* copies `usr/`, `etc/`, `lib/` into `/`;
* builds a Python 3.11+ venv at `/usr/libexec/nl-router/python/`;
* runs `systemctl daemon-reload` (skipped on hosts without systemd).

## 4. Configure

The postinstall scriptlet drops `/etc/nl-router/config.toml.example` and
copies it to `/etc/nl-router/config.toml` on first install only (subsequent
upgrades preserve operator edits). The minimum two values:

```toml
# /etc/nl-router/config.toml
[core]
server_id    = "node-a"
database_url = "postgres://nl_router:CHANGEME@localhost:5432/nl_router?sslmode=require"
```

`server_id` is the per-node identifier — each node only processes
work_queue rows tagged with its own `server_id`. Multi-node deployments
need a unique value per node.

The rest of the file has reasonable defaults; see the inline comments
for every knob.

Optional: configure TLS for the DICOM SCP, set custom landing-zone path,
adjust the back-pressure thresholds. All knobs are env-overridable —
see `python/nl_router/config.py` for the canonical list.

## 5. First-run setup

```sh
sudo nl-router init
```

The wizard, in order:
1. Generates `/etc/nl-router/kek.key` (32 random bytes, base64url-encoded,
   `0400 nl-router:nl-router`) if absent.
2. Runs all pending migrations.
3. Mints a bootstrap admin API token.
4. Prints the token **exactly once**.

Copy the `nlr_…` token before the terminal scrolls — the database stores
only its SHA-256 hash and there's no way to recover it.

The wizard is idempotent. Re-running it skips already-done steps (KEK
already present, schema current, active token exists). Use `--force` to
mint an additional token.

Useful skip flags: `--skip-kek-gen`, `--skip-migrate`, `--skip-token`.

## 6. Enable services

The package ships 7 systemd units. Enable the ones the node should run:

```sh
# Core daemon set on every node:
sudo systemctl enable --now \
    nl-router-receiver \
    nl-router-route \
    nl-router-dispatcher \
    nl-router-cleaner

# Management API (typically only on management nodes):
sudo systemctl enable --now nl-router-api

# Processing-module workers — one per module kind you use:
sudo systemctl enable --now nl-router-module@anonymize_basic
```

The templated `nl-router-module@<kind>` instance name MUST match the
`processing_modules.kind` column (underscores, not hyphens).

`nl-router-migrate.service` exists but is not enabled — it's a one-shot
operators run during deploys (`sudo systemctl start nl-router-migrate`).

## 7. Verify

```sh
# CLI works (via the bundled venv shim)
nl-router --version

# API health
curl -s http://localhost:8080/healthz

# Authenticated request
TOKEN='nlr_<paste>'
curl -s -H "Authorization: Bearer $TOKEN" http://localhost:8080/api/v1/rules

# Receiver listening on DICOM port
echoscu -aec NL_ROUTER -aet TEST localhost 11112

# Receiver Prometheus metrics
curl -s http://localhost:9180/metrics | head -20
```

## Container-only install (no host install)

Skip steps 3–6 entirely. Use the runtime container image directly.

### Via docker compose (dev / single-node)

```sh
BUILD_IMAGES=1 packaging/build-all.sh
docker tag nl-router:0.0.1-$(uname -m | sed 's/x86_64/amd64/; s/aarch64/arm64/') \
           nl-router:latest

docker compose --profile services up -d

# init runs as root inside the api container so it can write the KEK
# to /etc/nl-router. The persistent daemons stay unprivileged.
docker compose exec -u 0 api nl-router init
```

That brings up the full daemon stack (receiver, route, dispatcher,
cleaner, api, anonymize_basic worker) plus Postgres, PgBouncer, and
MinIO from local images.

### Via Kubernetes (production)

The runtime image dispatches roles via the first argument. Wire one
Deployment per role:

```yaml
# Excerpt — operator builds the full manifest.
spec:
  template:
    spec:
      containers:
        - name: receiver
          image: ghcr.io/your-org/nl-router:0.0.1
          args: ["receiver"]                      # → /usr/bin/nl-receiver
          env:
            - name: NL_ROUTER_SERVER_ID
              valueFrom: { fieldRef: { fieldPath: spec.nodeName } }
            - name: NL_ROUTER_DATABASE_URL
              valueFrom: { secretKeyRef: { name: nl-router, key: dsn } }
            - name: NL_ROUTER_KEK
              valueFrom: { secretKeyRef: { name: nl-router, key: kek } }
          volumeMounts:
            - { name: landing, mountPath: /var/lib/nl-router }
          ports:
            - { containerPort: 11112, name: dicom }
            - { containerPort: 9180,  name: metrics }
```

Available roles: `receiver`, `route`, `dispatcher`, `cleaner`, `api`,
`module <kind>`, `migrate <args>`, `init`, `cli <args>`. See
`packaging/runtime/entrypoint.sh` for the full dispatcher.

`init` is typically run as a Kubernetes Job (or `kubectl exec -ti
api -- nl-router init` once) rather than a Deployment.

## Upgrade

```sh
# Build the new package
ARCHES=amd64 packaging/build-all.sh

# Install over the existing one
sudo apt install ./packaging/dist/nl-router_0.0.2_amd64.deb   # or dnf, or tarball

# Apply any new migrations
sudo nl-router migrate up

# Restart the daemons to pick up the new binaries
sudo systemctl restart 'nl-router-*'
```

The postinstall rebuilds the bundled venv on every install/upgrade
(this is intentional — keeps the venv in lock-step with the wheel
ABI). It preserves `/etc/nl-router/`, `/var/lib/nl-router/`, and any
`config.toml` edits.

Downgrade: install the prior package, then `nl-router migrate down N`
(every migration ships an `up` and `down` pair).

## Uninstall

```sh
sudo systemctl disable --now 'nl-router-*'
sudo apt remove nl-router   # or dnf remove nl-router
```

The package's postremove leaves operator-owned state in place:
* `/etc/nl-router/` — config + KEK
* `/var/lib/nl-router/` — landing-zone files
* The `nl-router` system user

To purge those manually:

```sh
sudo rm -rf /etc/nl-router /var/lib/nl-router /var/log/nl-router
sudo userdel nl-router && sudo groupdel nl-router
```

Dropping the Postgres database is a separate step (`DROP DATABASE`).

## Filesystem layout

```
/usr/bin/
  nl-router                        Python CLI shim → bundled venv
  nl-receiver                      C++ DICOM SCP daemon
  nl-route                         C++ rule evaluator
  nl-dispatch                      C++ outbound sender
  nl-clean                         C++ retention cleaner
  nl-router-migrate                bundled golang-migrate binary

/usr/libexec/nl-router/
  modules/<kind>                   processing-module binaries
  python/                          venv built by postinstall
  wheels/nl_router-<ver>.whl       source wheel (kept for venv rebuild)

/usr/share/nl-router/
  migrations/*.sql                 consumed by nl-router-migrate

/lib/systemd/system/
  nl-router-receiver.service
  nl-router-route.service
  nl-router-dispatcher.service
  nl-router-cleaner.service
  nl-router-api.service
  nl-router-module@.service        templated, instance = kind
  nl-router-migrate.service        one-shot

/etc/nl-router/                    0750 root:nl-router
  config.toml                      copied from .example on first install
  config.toml.example
  kek.key                          0400 nl-router:nl-router

/var/lib/nl-router/                0750 nl-router:nl-router
  incoming/                        DICOM landing zone
  processing/                      processing-module intermediate output

/var/log/nl-router/                0750 nl-router:nl-router
```

The KEK at `/etc/nl-router/kek.key` is permission-checked at load time
(M13 hardening). The loader refuses to start if any group/other bits are
set on the file, or if it's owned by a user other than the effective UID
(when not running as root).

## Multi-node deployment

Each node runs the full daemon stack and processes only its own
`work_queue` rows (`server_id = self`). There is no work-stealing across
nodes — files live on local disk per node.

Recipe:

1. Provision an external Postgres (Patroni / RDS / Cloud SQL).
2. On each node:
   - Install the package (same steps 3-6).
   - Set a unique `server_id` in `/etc/nl-router/config.toml`.
   - Point all nodes at the same `database_url`.
   - Generate the KEK once on the first node, then copy it (with
     `0400 nl-router:nl-router`) to every other node. KEK rotation is a
     coordinated all-nodes operation.
3. Run `nl-router init` on the first node only (subsequent nodes already
   see the migrated schema; their `init` skips migration and refuses to
   mint a duplicate bootstrap token).

Management API runs on whichever nodes you choose. Put a load balancer
in front for HA.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `nl-router: bundled venv missing at /usr/libexec/nl-router/python` | postinstall failed; rerun `sudo dpkg --configure -a` or reinstall |
| `python3 ... is too old (need >= 3.11)` | distro default is too old; install python3.11 explicitly |
| `KEK file ... has insecure mode` | KEK is group/world-readable; `sudo chmod 0400 /etc/nl-router/kek.key` |
| `KEK file ... is owned by uid X but we are uid Y` | KEK ownership drifted; `sudo chown nl-router:nl-router /etc/nl-router/kek.key` |
| `No KEK configured` | first run before `nl-router init`, or KEK file deleted |
| `Schema is not initialized` | first run before `nl-router migrate up`; or `nl-router init` auto-applies |
| `Association Rejected: Local Limit Exceeded` | receiver at `max_associations` capacity or in disk-full reject; check `/metrics` |
| `Association Rejected` and `disk_guard.state_changed ... reject` | landing zone > `disk_reject_pct`; the cleaner frees space, then accepts resume automatically |
| Receiver crashes under load | this should be fixed in M13 (libpq mutex); if it reappears, capture a core dump and the `nl_receiver_db_insert_errors_total` metric |
| API returns 401 with a freshly-minted token | check the token was copied without surrounding whitespace; the `Authorization` header value must be exactly `Bearer nlr_<43chars>` |

## Further reading

* `packaging/README.md` — build pipeline internals
* `packaging/runtime/` — container image specifics
* `docker-compose.yml` — dev stack (Postgres + PgBouncer + MinIO + optional daemons)
* `python/nl_router/config.py` — the canonical list of every config knob
