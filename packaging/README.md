# packaging/

Build infrastructure for `nl-router` distribution artifacts.

## Outputs

| Format | Tool | Target |
|---|---|---|
| `.deb` (amd64, arm64) | [nfpm](https://github.com/goreleaser/nfpm) | Debian / Ubuntu |
| `.rpm` (x86_64, aarch64) | nfpm | RHEL / Rocky / Fedora / Amazon Linux |

Tarball + `install.sh` fallback and container images are M12.

## One-command build (recommended)

```sh
docker build -t nl-router-build -f packaging/Dockerfile.build packaging/
docker run --rm -v "$PWD":/src -w /src nl-router-build packaging/build.sh
ls packaging/dist/
```

The Dockerfile bakes every build dependency (DCMTK, libpq, OpenSSL,
nfpm, golang-migrate, Python 3.11) so the only host requirement is
Docker.

## Native build (Linux only)

If you already have the deps installed on a Debian/Ubuntu host:

```sh
sudo apt install -y build-essential cmake ninja-build \
                    libdcmtk-dev libpq-dev libssl-dev \
                    nlohmann-json3-dev libcurl4-openssl-dev \
                    python3 python3-venv python3-pip
pip install --user hatchling build
# Install nfpm + golang-migrate per their upstream docs.
packaging/build.sh
```

## Filesystem layout the package installs

```
/usr/bin/nl-router                      # Python CLI shim → bundled venv
/usr/bin/nl-receiver                    # C++ DICOM SCP
/usr/bin/nl-route                       # C++ rule evaluator
/usr/bin/nl-dispatch                    # C++ outbound sender
/usr/bin/nl-clean                       # C++ retention cleaner
/usr/bin/nl-router-migrate              # bundled golang-migrate

/usr/libexec/nl-router/
  modules/<kind>                        # processing module binaries
  python/                               # venv created by postinstall
  wheels/nl_router.whl                  # source wheel for the venv

/usr/share/nl-router/
  migrations/*.sql                      # consumed by nl-router-migrate

/lib/systemd/system/
  nl-router-receiver.service
  nl-router-route.service
  nl-router-dispatcher.service
  nl-router-cleaner.service
  nl-router-api.service
  nl-router-module@.service             # templated, instance = kind
  nl-router-migrate.service             # one-shot

/etc/nl-router/
  config.toml.example                   # operator copies → config.toml
  (config.toml created by postinstall on first install)
```

## Install / upgrade flow

```sh
sudo apt install ./packaging/dist/nl-router_<version>_amd64.deb
sudo $EDITOR /etc/nl-router/config.toml      # DSN, server_id
sudo nl-router migrate                        # apply schema
sudo nl-router init                           # mint admin API token
sudo systemctl enable --now nl-router-api \
                             nl-router-receiver \
                             nl-router-route \
                             nl-router-dispatcher \
                             nl-router-cleaner
```

## What postinstall does

1. Creates the `nl-router` system user/group (preinstall, actually).
2. Creates `/var/lib/nl-router/{incoming,processing}` and `/var/log/nl-router/`
   chowned to `nl-router:nl-router`.
3. Builds a Python venv at `/usr/libexec/nl-router/python/` from the
   host's `python3` and installs the bundled wheel into it.
4. `systemctl daemon-reload`. **Does not enable services** — operator
   runs `nl-router init` first.

## Why a venv instead of a wheel install at system level?

- Stability across upgrades — system Python deps change unpredictably.
- The receiver/router/etc. are C++ binaries with no Python dependency;
  the venv only services the CLI + API tier. A failed pip upgrade
  doesn't take down the DICOM hot path.
- Postinstall builds the venv from the host's `python3`, so the wheel
  is portable across Python 3.11/3.12/3.13 without per-version artifacts.
