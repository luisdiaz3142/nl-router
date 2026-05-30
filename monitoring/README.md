# nl-router monitoring stack

Self-contained Prometheus + Grafana for nl-router. Bring up the stack
and you get live dashboards over the metrics endpoints the daemons
expose at `:9180-:9184`. No external observability service required.

This is intended for **dev VMs and small single-node pilots**. For
shipping to production sites, see [Future: Datadog](#future-datadog)
below — the metric format is identical, so it's an ops-only swap.

## What's in the box

| Component | Port | Where |
|---|---|---|
| Prometheus | 9090 | scrapes the nl-router metric ports every 15 s |
| Grafana | 3000 | reads Prometheus, serves the provisioned dashboard |

Both run under host networking so they reach `localhost:9180-9184`
(core daemons) and `localhost:9190+` (module workers) as if they were
the host process. Persistent state lives in the named Docker volumes
`prometheus_data` and `grafana_data`.

## Prerequisites

- Docker (with `docker compose` v2). On dicom-diablo this is already
  installed for the M16 build pipeline.
- The nl-router daemons running on this host with their default
  metrics ports (9180-9184 for core daemons; 9190+ per module kind —
  see [Module-worker scrapes](#module-worker-scrapes) below).
  Override `NL_ROUTER_METRICS_PORT` in each systemd unit if your site
  uses different ports — then update `prometheus.yml` to match.

## Bring it up

```sh
cd ~/nl-router
make monitoring-up           # convenience wrapper
# or:
cd monitoring && docker compose up -d
```

First start downloads the two container images (~250 MB total) and
spins up the containers. After ~30 seconds:

- **Prometheus**: <http://your-host:9090> — try the `up` query to
  see scrape health per target.
- **Grafana**: <http://your-host:3000> — login `admin` / `admin`
  (you'll be prompted to change). Dashboard is under
  *Dashboards → nl-router → nl-router — overview*.

## Bring it down

```sh
make monitoring-down                       # keeps volumes
cd monitoring && docker compose down -v    # destroys TSDB + Grafana state
```

## Configuration

| File | What it controls |
|---|---|
| `docker-compose.yml` | container versions, ports, volumes, env |
| `prometheus.yml` | scrape targets, retention (default 30 d), `external_labels.server_id` |
| `grafana/provisioning/datasources/prometheus.yml` | the Prometheus datasource pointed at `localhost:9090` |
| `grafana/provisioning/dashboards/dashboards.yml` | provisioner config — picks up every JSON under `../dashboards/` |
| `../dashboards/*.json` | the actual dashboards (versioned with the rest of the repo) |

### Updating dashboards

Edit the JSON under `dashboards/`, commit, `git pull` on the host —
Grafana's file watcher picks the change up within 30 seconds. No
manual Import step.

### Changing `server_id`

`prometheus.yml`'s `external_labels.server_id` defaults to
`dicom-diablo`. Change it to whatever `NL_ROUTER_SERVER_ID` is on
this host so the dashboard's `$server_id` variable filters correctly.
After editing, hot-reload Prometheus without a container restart:

```sh
curl -X POST http://localhost:9090/-/reload
```

### Module-worker scrapes

`nl-router init` automatically assigns each shipped module kind a
metrics port (9190+) and writes
`/etc/nl-router/module-<kind>.env` with `NL_ROUTER_METRICS_PORT=<port>`.
The systemd template (`nl-router-module@.service`) sources that file
on each worker start, so the moment you enable the worker its
`/metrics` endpoint is live and Prometheus picks it up on the next
scrape.

Current shipped assignments:

| Kind | Port |
|---|---|
| `anonymize_basic` | 9190 |
| `standardize_institution_group` | 9191 |

To activate one:

```sh
sudo systemctl enable --now nl-router-module@anonymize_basic
```

(Mind the underscores — kinds use snake_case, not hyphens.)

The Prometheus scrape job for it is already in `prometheus.yml`.
Until the worker is enabled, the target shows `DOWN` — that's
expected and not an error.

### Adding a custom module-worker scrape

For a module kind nl-router doesn't ship:

1. Pick a port outside the assigned range (e.g. `9290`) and write
   it into `/etc/nl-router/module-<your-kind>.env`:
   ```ini
   NL_ROUTER_METRICS_PORT=9290
   ```
2. Enable the worker: `sudo systemctl enable --now nl-router-module@<your-kind>`
3. Add a scrape job to this `prometheus.yml`:
   ```yaml
   - job_name: nl-router-mod-<your-kind>
     static_configs:
       - targets: ["localhost:9290"]
         labels:
           service: module
           module_kind: <your-kind>
   ```
4. Hot-reload Prometheus: `curl -X POST http://localhost:9090/-/reload`

## What you should see immediately

Send a few studies through the receiver (e.g. `storescu` from
DCMTK), and within 15 seconds:

- **Receiver row** lights up: associations, instances received,
  duration histogram, studies closed.
- **Router row**: poll iterations, rows routed, claim latency.
- **Dispatcher row**: per-kind dispatch rate, duration p95,
  destinations active.
- **Per-AET breakdown row** (bottom): top callers by
  instances/bytes/s, association results segmented by AET — the
  panel that answers "which scanner is misbehaving?"

With no traffic, "No data" is expected.

## Future: Datadog

When this deployment graduates from dev to production at a site that
already uses Datadog, the migration is **ops-only — no nl-router code
changes**:

1. Install the Datadog Agent on the host.
2. Drop a `prometheus_check` (OpenMetricsCheck) config that points
   at the same `:9180-:9184/metrics` endpoints — every metric name
   and label set carries over verbatim.
3. Stop this docker-compose stack.

Sample Datadog Agent config snippet for the receiver port:

```yaml
# /etc/datadog-agent/conf.d/openmetrics.d/conf.yaml
instances:
  - openmetrics_endpoint: http://localhost:9180/metrics
    namespace: nl_router
    metrics: ["nl_receiver_*"]
    # one entry per port; the agent scrapes all five
```

The dashboards become Datadog dashboards (different JSON shape, but
the queries translate one-to-one). This README will get a "migrate
to Datadog" section when that work lands.

## Troubleshooting

**"No data" everywhere in Grafana but Prometheus shows targets are `UP`.**
Check the dashboard's `$server_id` variable — if Prometheus's
`external_labels.server_id` doesn't match what you selected in the
dropdown, every panel returns empty. Either change `prometheus.yml`
or pick `All` in the dropdown.

**`up{}` is 0 for one of the nl-router jobs.**
The corresponding daemon isn't running or its metrics port isn't
listening. Check `systemctl status nl-router-<service>` and verify
the daemon's `NL_ROUTER_METRICS_PORT` matches `prometheus.yml`.

**Grafana login fails with `admin/admin`.**
Either someone changed the password (look in `grafana_data` volume)
or the first-login flow already forced a reset. Reset via:
```sh
docker exec -it nl-router-grafana grafana-cli admin reset-admin-password <newpass>
```

**Want a clean slate.**
```sh
cd monitoring && docker compose down -v
docker compose up -d
```
Loses all historical metrics + any UI dashboard edits.
