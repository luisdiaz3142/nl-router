# End-to-end smoke test

The "did nl-router actually work?" recipe. Send a DICOM study at the
receiver, watch it walk through routing + dispatch, confirm it landed
at a real DICOM SCP. Run it after every fresh install, before every
release, and when something feels off.

Total runtime: **~5 minutes** on a working install. A failure at any
step localizes the bug to that one component.

---

## 0. Topology

```
┌─────────────────┐                  ┌──────────────────────┐
│ Your laptop     │  SSH tunnel      │  dicom-diablo (VM)   │
│                 │                  │                      │
│ storescu ───────┼──── :11112 ──────┼──→ nl-receiver       │
│                 │                  │     ↓                │
│ Browser ────────┼──── :8077  ──────┼──→ nl-router API     │
│ Browser ────────┼──── :3000  ──────┼──→ Grafana           │
│ Browser ────────┼──── :9090  ──────┼──→ Prometheus        │
│                 │                  │     ↓                │
│ Orthanc :8042 ←─┼──── :4242  ──────┼─── nl-dispatcher     │
│ (laptop-local)  │   (reverse R)    │                      │
└─────────────────┘                  └──────────────────────┘
```

Adjust if your Orthanc lives elsewhere — the only requirement is that
the dispatcher can reach the Orthanc DIMSE port (4242) by hostname:port.

---

## 1. Prerequisites

### On the VM

| Check | Command | Expected |
|---|---|---|
| .deb installed | `dpkg -s nl-router \| grep Status` | `install ok installed` |
| Init ran | `ls -l /etc/nl-router/{kek.key,env}` | both exist, `kek.key` 0400 |
| Postgres up | `pg_isready -h localhost -p 5433` | `accepting connections` |
| All services active | `systemctl is-active nl-router-{api,receiver,route,dispatcher,cleaner}` | all `active` |
| DICOM port listening | `ss -tuln \| grep ':11112'` | one `LISTEN` line |
| Metrics ports listening | `ss -tuln \| grep -E ':918[0-3]\|:9184'` | five `LISTEN` lines |

If any line fails, jump to [Gotchas](#gotchas) before continuing.

### On your laptop

```bash
# DCMTK tooling (one-time install)
brew install dcmtk                              # macOS
sudo apt install dcmtk                          # Debian/Ubuntu

# Orthanc — fast, free DICOM SCP for testing
docker run -d --name orthanc-test \
    -p 4242:4242 -p 8042:8042 \
    jodogne/orthanc
```

Verify Orthanc is up:

```bash
curl -sf http://localhost:8042/system | head -1     # should return JSON
```

---

## 2. Open the tunnels

One SSH session that forwards everything you need (adjust hostname for
your environment):

```bash
gcloud compute ssh dicom-diablo --tunnel-through-iap -- -N \
    -L 8077:localhost:8080 \
    -L 3000:localhost:3000 \
    -L 9090:localhost:9090 \
    -L 11112:localhost:11112 \
    -R 4242:localhost:4242
```

Plain SSH equivalent (skip the gcloud wrapper if you're not on GCP):

```bash
ssh -N \
    -L 8077:localhost:8080 \
    -L 3000:localhost:3000 \
    -L 9090:localhost:9090 \
    -L 11112:localhost:11112 \
    -R 4242:localhost:4242 \
    dicom-diablo
```

Leave that session running. Ctrl-C closes all forwards.

What each does:

- `-L 8077:…:8080` — laptop's `:8077` → VM's API at `:8080` (avoid the
  `:8080` collision with whatever else you run locally)
- `-L 3000`/`9090` — Grafana + Prometheus
- `-L 11112` — laptop's `:11112` → VM's DICOM receiver
- `-R 4242` — VM's `:4242` → laptop's Orthanc (the reverse forward)

---

## 3. Configure nl-router

One-time setup. Skip this if it's already done.

### 3.1. Create the Orthanc destination

Open the UI: <http://localhost:8077/ui> → sign in with your API token
(printed by `nl-router init`, or under Sidebar → Credentials).

Sidebar → **Destinations** → **+ New destination**:

| Field | Value |
|---|---|
| Name | `ORTHANC` |
| Kind | `dicom` |
| Enabled | ✓ |
| Concurrency | `4` |
| Config | (see below) |

```json
{
  "host": "127.0.0.1",
  "port": 4242,
  "called_aet": "ORTHANC",
  "calling_aet": "NL_ROUTER",
  "max_pdu_size": 131072,
  "preferred_transfer_syntaxes": [
    "1.2.840.10008.1.2.1",
    "1.2.840.10008.1.2"
  ],
  "tls": false
}
```

Save → click **Test connection**. Should show:

> ✓ **Connection OK** dicom · *X* ms — TCP connect to 127.0.0.1:4242 succeeded.

A red ✗ here means the reverse tunnel isn't routing — verify Orthanc
is up on your laptop and the `-R 4242:localhost:4242` flag is in your
SSH command.

### 3.2. Create a rule

Sidebar → **Rules** → **+ New rule**:

| Field | Value |
|---|---|
| Name | `route-all-to-orthanc` |
| Scope | `study` |
| Status | `enabled` |
| Priority | `0` |
| Dispatch order | `parallel` |
| Predicate | `true` |

> The predicate `true` matches every study — fine for smoke testing.
> Production rules filter on tags like `tags.Modality == "CT"`.

Save → scroll down to **Destination bindings** → **+ Bind a
destination** → pick `ORTHANC`, ordinal `0`, Bind.

### 3.3. Verify the router picked up the rule

```bash
sudo journalctl -u nl-router-route -n 5 --no-pager | grep rule_cache
```

Expected (within ~15 seconds):

> `rule_cache.refreshed parsed=1 failed=0 study=1`

If `failed >= 1`, your predicate didn't parse — the UI should have
caught this at Save time (M22). If `parsed=0`, the rule isn't
enabled or the router hasn't refreshed yet (wait 15s).

---

## 4. Send a study

```bash
storescu -aec NL_ROUTER -aet TEST_MOD +r +sd \
    localhost 11112 \
    ~/path/to/study/
```

| Flag | Meaning |
|---|---|
| `-aec NL_ROUTER` | called AE Title (matches the receiver's config) |
| `-aet TEST_MOD` | calling AE Title (whatever you want; shows up in metrics) |
| `+r` | recurse subdirectories |
| `+sd` | treat input as a directory of DICOM files |
| `localhost 11112` | through the tunnel to the VM's receiver |

Expected output: progress lines and `Releasing Association`. No errors.

---

## 5. Verify

### 5.1. nl-router UI

<http://localhost:8077/ui/studies> — your study appears immediately.
Watch the status pill walk:

| Status | When |
|---|---|
| `received` | Right after storescu finishes |
| `routed` | ~Few seconds later (assoc_end close) — or wait up to 60s for study-idle timer |
| `dispatching` → `dispatched` | Seconds after `routed` |

**Click the row** for the study detail page. The **Route assignments**
table should have one row pointing at `ORTHANC` with status
`dispatched` and a timestamp. The Timeline card shows each phase's
exact time.

### 5.2. Grafana

<http://localhost:3000/> — login `admin` / your password →
**Dashboards** → **nl-router** → **nl-router — overview**.

Within ~15 seconds (one Prometheus scrape interval) you should see:

- **Receiver row**: `Associations` and `Instances received / s` panels
  show a spike. `Studies closed by trigger` shows `assoc_end` (or
  `study_timer`).
- **Router row**: `Rows routed / s` ticks up by 1.
- **Dispatcher row**: `Assignments by kind / result` shows
  `dicom / dispatched` ticking up.
- **Per-AET breakdown row** (bottom): your `TEST_MOD` AET shows up
  in the top-callers panel.

### 5.3. Orthanc

<http://localhost:8042/> — Orthanc's web UI. Click **"All studies"** in
the top nav. Your study should appear with patient name, modality,
study date.

If you also need to verify via API:

```bash
curl -s http://localhost:8042/studies | jq length        # count of studies
curl -s http://localhost:8042/studies | jq '.[0]'         # first study uuid
```

---

## 6. Cleanup

When you're done:

```bash
# Stop Orthanc
docker stop orthanc-test && docker rm orthanc-test

# Close the tunnels: Ctrl-C the SSH session

# Optional: delete the test rule + destination via the UI so they
# don't fire on real traffic.
```

The study row in nl-router stays in `dispatched` status. The cleaner
will eventually delete its files based on retention policy; nothing
to do here.

---

## Gotchas

Things that bit us during real testing. If your smoke test fails, scan
this list first.

### Daemons crash-loop with `required env var not set: NL_ROUTER_SERVER_ID`

You installed the .deb but didn't run `nl-router init`. The systemd
unit looks for `/etc/nl-router/env` which init generates from
`config.toml`. Fix:

```bash
sudo nl-router init                              # generates env file + KEK
sudo systemctl reset-failed 'nl-router-*'        # clear crash counters
sudo systemctl restart 'nl-router-*.service'
```

### `storescu` connects but the study never appears in the UI

The receiver's metrics will tick but the work_queue row never
materializes if the study close trigger doesn't fire. Check:

```bash
sudo journalctl -u nl-router-receiver -n 30 --no-pager | grep study
```

Look for `study.closed` — that's the line that means a row is about
to be inserted. If you don't see it within ~70 seconds of storescu
finishing, the study-idle timer isn't running (M10 bug — file a
ticket).

### Status stuck at `routed`, Route assignments is `0`

The router evaluated the row but no rule matched. Most common causes:

1. **Predicate didn't parse.** Pre-M22 this was silent; M22 reports it
   at Save time. Verify with:
   ```bash
   sudo journalctl -u nl-router-route -n 5 --no-pager | grep rule_cache
   ```
   `failed > 0` means a rule didn't parse — check Rules page for an
   error indicator, edit + re-save the broken predicate.

2. **Rule status isn't `enabled`.** Drafts and disabled rules never
   evaluate. Open Rules list; the pill should be green.

3. **Rule has no destinations bound.** Open the rule's edit page;
   the Destination bindings table should have ≥ 1 row.

4. **Row was routed before the rule existed.** Once `routed`, the
   router never re-evaluates. Send a fresh study.

### Status reaches `dispatched` but study isn't in Orthanc

Click the study row, look at the **Route assignments** table's
error column. Common reasons:

- **Reverse SSH tunnel dropped.** The dispatcher attempted the
  C-STORE but TCP connect failed. Re-establish your SSH session.
- **Orthanc rejected the C-STORE.** Most often: AET allowlist
  enabled and `NL_ROUTER` isn't on it. Check Orthanc's config or
  flip `DicomAlwaysAllowStore` to `true`.
- **Transfer syntax mismatch.** Orthanc's defaults accept most
  syntaxes; if you've narrowed yours, the dispatcher's
  `preferred_transfer_syntaxes` may not overlap.

### Grafana shows "No data" for every panel

Either the Prometheus scrape isn't working or the dashboard's
`$server_id` filter doesn't match.

1. Check Prometheus targets are healthy:
   <http://localhost:9090/targets> → every `nl-router-*` job should
   say `UP`. If anything's red, that daemon's metrics port isn't
   listening — restart the daemon.
2. Check the dashboard's `Server` dropdown at the top — if it's set
   to a server_id that doesn't match `external_labels.server_id` in
   `monitoring/prometheus.yml`, panels show nothing. Switch to `All`
   or edit prometheus.yml to match.

### Test connection button reports "TCP connect succeeded" but DICOM still fails

The DICOM-kind probe (M19) does **TCP-only reachability** — it confirms
something is listening on host:port but not that DIMSE negotiation
works. A working TCP probe + a failing real dispatch usually means
AET mismatch or transfer syntax mismatch. C-ECHO support is a
v2 follow-up.

---

## Success criteria checklist

Run this after every install/upgrade. Copy + paste into a Linear
ticket comment when reporting a release tested.

```
[ ] All 5 systemd services active
[ ] `ss -tuln` shows 11112 + 9180..9184 + 8080 listening
[ ] Orthanc reachable: curl http://localhost:8042/system → 200
[ ] Destination "Test connection" returns green
[ ] Rule cache: parsed >= 1, failed = 0 in router journal
[ ] Send 1 study via storescu → no error in storescu output
[ ] UI Studies row reaches `dispatched` status within 2 minutes
[ ] Grafana Receiver row shows a spike for the test AET
[ ] Study visible in Orthanc web UI under "All studies"
```

If every line passes, nl-router works end-to-end on this host.
