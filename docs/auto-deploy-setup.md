# Auto-deploy setup

The `.github/workflows/deploy.yml` workflow auto-deploys nl-router
to a GCP-hosted VM on every green CI run on `main`. It does the
same work as `make redeploy HOST=...` but triggered by CI instead
of a human typing the command.

The workflow ships **disabled by default** — it'll print a notice
in the run log and skip every deploy until you configure the three
GitHub-side values below. That keeps it safe to merge before
you've finished the GCP-side setup.

## What gets deployed

Same artifact `make redeploy` produces locally:

1. The `build` workflow builds a `.deb`, runs the packaging smoke
   test, and uploads the file as the `nl-router-deb-amd64` artifact.
2. The `deploy` workflow downloads that exact artifact, scp's it +
   `remote-install.sh` to the host via IAP, and runs the install.
3. Service health is asserted before the workflow exits non-zero
   (so a deploy that breaks a service shows up as a red run in
   the Actions tab).

## Prerequisites

- A GCP project containing the deploy host.
- The deploy host is reachable via IAP (Identity-Aware Proxy) —
  this is the default for GCP Compute Engine VMs without a public
  IP.
- You can `gcloud compute ssh --tunnel-through-iap <host>` from
  your laptop today. (If you can't, fix that first — the workflow
  uses the same code path.)

## One-time GCP setup (~10 min)

### 1. Create a service account

```sh
PROJECT=your-gcp-project-id
HOST=dicom-diablo            # or whatever the VM is named
ZONE=us-central1-a

gcloud iam service-accounts create nl-router-deploy \
    --project="$PROJECT" \
    --description="GitHub Actions auto-deploy of nl-router" \
    --display-name="nl-router deploy"

SA_EMAIL="nl-router-deploy@${PROJECT}.iam.gserviceaccount.com"
```

### 2. Grant the SA the roles it needs

Minimum permissions to SSH into a VM via IAP, copy files, and run
sudo commands. The `--condition=None` flag suppresses the
interactive prompt that gcloud throws when your project already
has conditional IAM bindings — we want each role here
unconditional.

```sh
# Talk to the Compute Engine API
gcloud projects add-iam-policy-binding "$PROJECT" \
    --member="serviceAccount:${SA_EMAIL}" \
    --role="roles/compute.osLogin" \
    --condition=None

# Tunnel through IAP
gcloud projects add-iam-policy-binding "$PROJECT" \
    --member="serviceAccount:${SA_EMAIL}" \
    --role="roles/iap.tunnelResourceAccessor" \
    --condition=None

# Read instance metadata (gcloud needs this for compute ssh)
gcloud projects add-iam-policy-binding "$PROJECT" \
    --member="serviceAccount:${SA_EMAIL}" \
    --role="roles/compute.viewer" \
    --condition=None
```

`roles/compute.osLogin` gives the SA a Linux user on the host via
[OS Login](https://cloud.google.com/compute/docs/oslogin). To also
let it use `sudo` (required by `remote-install.sh`), grant
`roles/compute.osAdminLogin` instead — but only if you're
comfortable with the SA having root on the host:

```sh
gcloud projects add-iam-policy-binding "$PROJECT" \
    --member="serviceAccount:${SA_EMAIL}" \
    --role="roles/compute.osAdminLogin" \
    --condition=None
```

If you hit a prompt like *"The policy contains bindings with
conditions, so specifying a condition is required..."*, the
`--condition=None` flag is what you missed. Answer `None` from
the menu and re-run.

### 3. Enable OS Login on the host (if not already)

```sh
gcloud compute instances add-metadata "$HOST" \
    --zone="$ZONE" --project="$PROJECT" \
    --metadata enable-oslogin=TRUE
```

### 4. Mint a JSON key for the SA

⚠️ **Long-lived JSON keys are not GCP's recommended auth pattern.**
For an initial setup they're the simplest path; once auto-deploy
is working, see the [WIF migration](#workload-identity-federation-recommended-upgrade)
section below to replace this with short-lived federated tokens.

```sh
gcloud iam service-accounts keys create /tmp/nl-router-deploy.json \
    --iam-account="$SA_EMAIL" \
    --project="$PROJECT"

# Print the key so you can copy it into GitHub Secrets.
cat /tmp/nl-router-deploy.json
```

**Delete the local copy** after pasting into GitHub:

```sh
shred -u /tmp/nl-router-deploy.json   # or just rm
```

## GitHub-side setup (~2 min)

Repo → **Settings → Secrets and variables → Actions**

### Add the secret

| Type | Name | Value |
|---|---|---|
| Secret | `GCP_SA_KEY` | The full JSON contents from the previous step |

### Add the variables

| Type | Name | Example value |
|---|---|---|
| Variable | `GCP_PROJECT` | `your-gcp-project-id` |
| Variable | `DEPLOY_HOST` | `dicom-diablo` |
| Variable | `DEPLOY_ZONE` (optional) | `us-central1-a` (defaults to this if unset) |

## Try it

Push a small commit to `main`. Within ~5-10 minutes the timeline
should look like:

1. GitHub Actions → `build` workflow runs (~5 min).
2. On green, `deploy` workflow fires.
3. `deploy` downloads the artifact, auths to GCP, scp's + ssh's
   via IAP, runs `remote-install.sh`.
4. The deploy host comes up on the new commit; all services
   `active` after restart.

Watch both workflows in the Actions tab. If `deploy` shows a red
run, click into the failing step — the gcloud errors are usually
self-explanatory (permission missing, host not found, etc.).

## Troubleshooting

### `deploy` is skipped with a "not configured" notice

One of `GCP_SA_KEY` / `GCP_PROJECT` / `DEPLOY_HOST` is missing.
Verify all three are set under repo Settings → Secrets and
variables → Actions.

### `Permission denied (publickey)` from gcloud

The SA doesn't have OS Login Linux access on the host. Verify:

```sh
gcloud compute os-login describe-profile \
    --impersonate-service-account="$SA_EMAIL"
```

If the profile is empty, the SA hasn't been granted
`roles/compute.osLogin` or the host doesn't have OS Login enabled.

### `sudo: a password is required` in the remote-install step

OS Login granted via `compute.osLogin` doesn't include sudo.
Either:

- Grant `compute.osAdminLogin` (gives SA root)
- Or pre-add the SA's OS Login user to `/etc/sudoers.d/nl-router-deploy`
  with `NOPASSWD: ALL`

### Deploy succeeds but the running version doesn't change

Look at the `Verify .deb contents` step in the corresponding
`build` run — if `nl-router_*.deb` didn't include your changes
(e.g. you forgot to add a new path to `nfpm.yaml`), `apt install
--reinstall` silently reinstalls the same content.

## Workload Identity Federation (recommended upgrade)

Long-lived SA JSON keys violate two best practices: they're
long-lived, and they're shared with a third party (GitHub). WIF
lets the workflow exchange a GitHub-issued JWT for a short-lived
GCP token, with no static credentials anywhere.

Migration steps (one-time):

1. Create a Workload Identity Pool + Provider for GitHub:
   ```sh
   gcloud iam workload-identity-pools create github-pool \
       --location=global --project="$PROJECT"
   gcloud iam workload-identity-pools providers create-oidc github-provider \
       --location=global \
       --workload-identity-pool=github-pool \
       --issuer-uri=https://token.actions.githubusercontent.com \
       --attribute-mapping="google.subject=assertion.sub,attribute.repository=assertion.repository" \
       --attribute-condition="assertion.repository == 'YOUR_ORG/nl-router'"
   ```
2. Bind the existing SA to the pool:
   ```sh
   gcloud iam service-accounts add-iam-policy-binding "$SA_EMAIL" \
       --role=roles/iam.workloadIdentityUser \
       --member="principalSet://iam.googleapis.com/projects/PROJECT_NUMBER/locations/global/workloadIdentityPools/github-pool/attribute.repository/YOUR_ORG/nl-router"
   ```
3. In `.github/workflows/deploy.yml`, replace `credentials_json:`
   with:
   ```yaml
   workload_identity_provider: projects/PROJECT_NUMBER/locations/global/workloadIdentityPools/github-pool/providers/github-provider
   service_account: nl-router-deploy@PROJECT.iam.gserviceaccount.com
   ```
4. Delete the `GCP_SA_KEY` secret + delete the SA's JSON key:
   ```sh
   gcloud iam service-accounts keys list --iam-account="$SA_EMAIL"
   gcloud iam service-accounts keys delete KEY_ID --iam-account="$SA_EMAIL"
   ```

After that, no GitHub Secret contains anything sensitive — the
JWT exchange happens at workflow run time.
