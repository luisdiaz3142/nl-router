# nl-router Grafana dashboards

Shipped dashboards live here as Grafana 10.x-compatible JSON. They assume a
Prometheus datasource and the per-service `/metrics` endpoints documented
in the design plan.

## Files

| File | Scope |
| --- | --- |
| `nl-router-overview.json` | Single dashboard with one row per service (Receiver, Router, Dispatcher, Cleaner, Module workers, Management API). Suitable as a default landing page. |

Per-service deep-dive dashboards are deferred — the overview is enough to
catch a degraded daemon at a glance, and each service's metric catalog is
documented inline in its `metrics.{hpp,cpp}` (or `metrics.py` for the API).

## Importing

Grafana → Dashboards → New → Import → upload `nl-router-overview.json`.

Pick your Prometheus datasource when prompted. The dashboard exposes two
variables:

- `$datasource` — Prometheus datasource (auto-discovered).
- `$server_id` — filters every panel to one (or many) `server_id` values.
  Defaults to `All`. Populated from `label_values(nl_receiver_associations_total, server_id)`.

`server_id` is **not** emitted by the daemons themselves — they expose
unlabeled metrics. The `server_id` label comes from Prometheus relabel
rules (see below). If you haven't configured those, set `$server_id` to
`All` and the dashboard works fine; you just can't slice by node.

## Prometheus scrape config

The native daemons each expose `/metrics` on a dedicated port (defaults
from the design plan):

| Service | Default port |
| --- | --- |
| `nl-receiver` | 9180 |
| `nl-route` | 9181 |
| `nl-dispatch` | 9182 |
| `nl-clean` | 9183 |
| FastAPI management API | 9184 |
| Module workers | 9190+ (operator-assigned, one per kind) |

A minimal `prometheus.yml` job per node — replace `nl-host-1` with the
node's `NL_ROUTER_SERVER_ID`:

```yaml
scrape_configs:
  - job_name: nl-router
    static_configs:
      - targets:
          - "nl-host-1:9180"   # receiver
          - "nl-host-1:9181"   # router
          - "nl-host-1:9182"   # dispatcher
          - "nl-host-1:9183"   # cleaner
          - "nl-host-1:9184"   # api
        labels:
          server_id: nl-host-1
    relabel_configs:
      # Map each target's port to a human-readable `service` label.
      - source_labels: [__address__]
        regex: ".*:9180"
        target_label: service
        replacement: receiver
      - source_labels: [__address__]
        regex: ".*:9181"
        target_label: service
        replacement: router
      - source_labels: [__address__]
        regex: ".*:9182"
        target_label: service
        replacement: dispatcher
      - source_labels: [__address__]
        regex: ".*:9183"
        target_label: service
        replacement: cleaner
      - source_labels: [__address__]
        regex: ".*:9184"
        target_label: service
        replacement: api

  # Module workers — separate job because each binary picks a different
  # port and you want `module_kind` derived from the target.
  - job_name: nl-router-modules
    static_configs:
      - targets: ["nl-host-1:9190"]
        labels: { server_id: nl-host-1, module_kind: anonymize_basic }
      - targets: ["nl-host-1:9191"]
        labels: { server_id: nl-host-1, module_kind: standardize_institution_group }
```

For multiple nodes, duplicate each `static_configs` block with the right
host + `server_id`. Service discovery (`file_sd_configs`, Consul, K8s SD)
works the same way — just emit the `server_id` label on each target.

## Metric catalog (quick reference)

Inline catalogs live with the code:

- `cpp/receiver/src/metrics.{hpp,cpp}` — receiver metrics
- `cpp/router/src/metrics.{hpp,cpp}` — router metrics
- `cpp/dispatcher/src/metrics.{hpp,cpp}` — dispatcher metrics
- `cpp/cleaner/src/metrics.{hpp,cpp}` — cleaner metrics
- `cpp/modules/common/src/worker.cpp` — module-worker metrics (shared
  catalog used by every `nl-mod-*` binary)
- `python/nl_router/api/metrics.py` — management API metrics

Every metric carries its `help` string inline in those files; that is the
canonical reference.

## Editing the JSON

Grafana's UI exports a re-formatted JSON that often reorders fields and
introduces dashboard-specific UIDs. To stay diffable:

1. Edit in Grafana.
2. Use **Dashboard settings → JSON Model → Copy to clipboard**.
3. Paste over the file here.
4. Strip any `id`, `iteration`, or `version` fields that drift on every
   save; leave `uid` stable so import links keep working.

Or hand-edit this JSON directly. Schema version 38 corresponds to
Grafana 10.0+; newer Grafana releases auto-migrate older schemas
forward on first load.
