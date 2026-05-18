"""UI: destinations list + create/edit + delete."""

from __future__ import annotations

import json
from typing import Annotated

from fastapi import APIRouter, Depends, Form, HTTPException, Request
from fastapi.responses import RedirectResponse, Response

from nl_router.api.auth import AuthContext
from nl_router.db import pool
from nl_router.ui.auth import ui_auth_required
from nl_router.ui.common import render, set_flash


router = APIRouter(prefix="/ui/destinations", tags=["ui"], include_in_schema=False)


# Per-kind example config + short description shown next to the JSON textarea.
# Keeps operators from having to flip back to the README to remember the shape.
KIND_HELP: dict[str, tuple[str, str]] = {
    "dicom": (
        "DICOM C-STORE SCU. Opens one association per study and streams every instance.",
        json.dumps({
            "host": "10.0.1.5",
            "port": 11112,
            "called_aet": "ARCHIVE",
            "calling_aet": "NL_ROUTER",
            "max_pdu_size": 131072,
            "preferred_transfer_syntaxes": ["1.2.840.10008.1.2.1", "1.2.840.10008.1.2"],
            "tls": False,
        }, indent=2),
    ),
    "dicomweb_stow": (
        "DICOMweb STOW-RS POST. Auth via the linked credential (bearer / basic / mTLS).",
        json.dumps({
            "url": "https://pacs.example.com/dicom-web/studies",
            "accept": "application/dicom+json",
            "transfer_syntax": "1.2.840.10008.1.2.1",
        }, indent=2),
    ),
    "gcp_dicomweb": (
        "GCP Healthcare DICOMweb. credential_id must point at a gcp_service_account credential.",
        json.dumps({
            "project_id": "my-gcp-project",
            "location": "us-central1",
            "dataset": "radiology",
            "dicom_store": "main",
            "scope": "https://www.googleapis.com/auth/cloud-platform",
        }, indent=2),
    ),
    "http": (
        "Generic HTTP webhook. URL + body templating with ${TagName} substitution.",
        json.dumps({
            "url_template": "https://hook.example.com/dicom/${StudyInstanceUID}",
            "method": "POST",
            "headers": {"Content-Type": "application/json"},
            "body_template": '{"study_uid":"${StudyInstanceUID}","patient_id":"${PatientID}"}',
            "include_dicom_files": False,
        }, indent=2),
    ),
    "file": (
        "Local or mounted filesystem write. path_template supports ${TagName} substitution.",
        json.dumps({
            "path_template": "/archive/${PatientID}/${StudyInstanceUID}/",
            "fsync": True,
            "preserve_hierarchy": True,
        }, indent=2),
    ),
    "object_storage": (
        "S3-compatible PUT (AWS / GCS-XML / MinIO). credential_id should reference an aws_keys credential.",
        json.dumps({
            "endpoint": "https://s3.amazonaws.com",
            "bucket": "dicom-archive",
            "key_template": "${Modality}/${StudyDate}/${StudyInstanceUID}/${SOPInstanceUID}.dcm",
            "region": "us-east-1",
            "storage_class": "STANDARD_IA",
        }, indent=2),
    ),
}

DEFAULT_RETRY_POLICY = json.dumps({
    "max_attempts":        5,
    "initial_backoff_s":   30,
    "multiplier":          2.0,
    "max_backoff_s":       3600,
    "give_up_after_hours": 72,
}, indent=2)


# ---- List --------------------------------------------------------------


@router.get("", response_class=Response)
async def list_destinations(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT d.id, d.name, d.description, d.kind, d.enabled,
                   d.dispatch_concurrency, d.credential_id, c.name AS credential_name,
                   d.updated_at,
                   (SELECT COUNT(*) FROM rule_destinations rd WHERE rd.destination_id = d.id) AS rule_count
              FROM destinations d
              LEFT JOIN credentials c ON c.id = d.credential_id
             ORDER BY d.name
        """)
        rows = list(cur.fetchall())
    return render(request, "destinations_list.html", auth=auth, destinations=rows)


# ---- Create ------------------------------------------------------------


@router.get("/new", response_class=Response)
async def new_destination_page(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    kind: str = "dicom",
):
    creds = _load_credentials()
    return render(
        request, "destinations_form.html",
        auth=auth, mode="create",
        destination=_empty_destination(kind),
        errors={},
        kind_help=KIND_HELP,
        credentials=creds,
        default_retry_policy=DEFAULT_RETRY_POLICY,
    )


@router.post("", response_class=Response)
async def create_destination(
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    name:                 Annotated[str, Form()],
    kind:                 Annotated[str, Form()],
    config:               Annotated[str, Form()],
    description:          Annotated[str, Form()] = "",
    enabled:              Annotated[str, Form()] = "on",
    dispatch_concurrency: Annotated[int, Form()] = 4,
    credential_id:        Annotated[str, Form()] = "",
    retry_policy:         Annotated[str, Form()] = "",
):
    errors, config_obj, retry_obj = _validate_destination_form(
        name=name, kind=kind, config=config, retry_policy=retry_policy,
        dispatch_concurrency=dispatch_concurrency,
    )
    if errors:
        return render(
            request, "destinations_form.html",
            auth=auth, mode="create",
            destination={"name": name, "description": description, "kind": kind,
                         "enabled": enabled == "on", "config": config,
                         "dispatch_concurrency": dispatch_concurrency,
                         "credential_id": int(credential_id) if credential_id else None,
                         "retry_policy": retry_policy},
            errors=errors, kind_help=KIND_HELP,
            credentials=_load_credentials(),
            default_retry_policy=DEFAULT_RETRY_POLICY,
        )

    cid = int(credential_id) if credential_id else None
    enabled_bool = enabled == "on"

    with pool().connection() as conn, conn.cursor() as cur:
        try:
            cur.execute("""
                INSERT INTO destinations (
                    name, description, kind, enabled, config,
                    credential_id, dispatch_concurrency, retry_policy
                ) VALUES (%s, %s, %s, %s, %s::jsonb, %s, %s, %s::jsonb)
                RETURNING id
            """, (name, description or None, kind, enabled_bool,
                  json.dumps(config_obj), cid, dispatch_concurrency,
                  json.dumps(retry_obj) if retry_obj else DEFAULT_RETRY_POLICY))
            new_id = cur.fetchone()["id"]
            _audit(cur, auth, "destination.create", new_id,
                   {"after": {"name": name, "kind": kind, "enabled": enabled_bool}})
            conn.commit()
        except Exception as e:
            conn.rollback()
            msg = str(e).lower()
            if "duplicate" in msg or "unique" in msg:
                errors["name"] = f"A destination named {name!r} already exists."
            else:
                errors["__form__"] = f"Database error: {e}"
            return render(
                request, "destinations_form.html",
                auth=auth, mode="create",
                destination={"name": name, "description": description, "kind": kind,
                             "enabled": enabled_bool, "config": config,
                             "dispatch_concurrency": dispatch_concurrency,
                             "credential_id": cid, "retry_policy": retry_policy},
                errors=errors, kind_help=KIND_HELP,
                credentials=_load_credentials(),
                default_retry_policy=DEFAULT_RETRY_POLICY,
            )

    resp = RedirectResponse(url=f"/ui/destinations/{new_id}", status_code=303)
    set_flash(resp, f"Destination {name!r} created.")
    return resp


# ---- Edit --------------------------------------------------------------


@router.get("/{did}", response_class=Response)
async def edit_destination_page(
    did: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("""
            SELECT id, name, description, kind, enabled,
                   config, credential_id, dispatch_concurrency, retry_policy,
                   created_at, updated_at
              FROM destinations
             WHERE id = %s
        """, (did,))
        d = cur.fetchone()
        if not d:
            raise HTTPException(404, "destination not found")

    # Convert JSONB columns to indented strings for the textareas.
    d = dict(d)
    d["config"]       = json.dumps(d["config"],       indent=2) if d["config"]       else ""
    d["retry_policy"] = json.dumps(d["retry_policy"], indent=2) if d["retry_policy"] else ""

    return render(
        request, "destinations_form.html",
        auth=auth, mode="edit", destination=d, errors={},
        kind_help=KIND_HELP,
        credentials=_load_credentials(),
        default_retry_policy=DEFAULT_RETRY_POLICY,
    )


@router.post("/{did}", response_class=Response)
async def update_destination(
    did: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
    name:                 Annotated[str, Form()],
    kind:                 Annotated[str, Form()],
    config:               Annotated[str, Form()],
    description:          Annotated[str, Form()] = "",
    enabled:              Annotated[str, Form()] = "",
    dispatch_concurrency: Annotated[int, Form()] = 4,
    credential_id:        Annotated[str, Form()] = "",
    retry_policy:         Annotated[str, Form()] = "",
):
    errors, config_obj, retry_obj = _validate_destination_form(
        name=name, kind=kind, config=config, retry_policy=retry_policy,
        dispatch_concurrency=dispatch_concurrency,
    )
    if errors:
        return render(
            request, "destinations_form.html",
            auth=auth, mode="edit",
            destination={"id": did, "name": name, "description": description, "kind": kind,
                         "enabled": enabled == "on", "config": config,
                         "dispatch_concurrency": dispatch_concurrency,
                         "credential_id": int(credential_id) if credential_id else None,
                         "retry_policy": retry_policy},
            errors=errors, kind_help=KIND_HELP,
            credentials=_load_credentials(),
            default_retry_policy=DEFAULT_RETRY_POLICY,
        )

    cid = int(credential_id) if credential_id else None
    enabled_bool = enabled == "on"

    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT * FROM destinations WHERE id = %s", (did,))
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, "destination not found")
        cur.execute("""
            UPDATE destinations SET
                name = %s, description = %s, kind = %s,
                enabled = %s, config = %s::jsonb,
                credential_id = %s, dispatch_concurrency = %s,
                retry_policy = %s::jsonb,
                updated_at = NOW()
             WHERE id = %s
        """, (name, description or None, kind, enabled_bool,
              json.dumps(config_obj), cid, dispatch_concurrency,
              json.dumps(retry_obj) if retry_obj else None, did))
        _audit(cur, auth, "destination.update", did, {
            "before": {"name": prior["name"], "kind": prior["kind"]},
            "after":  {"name": name, "kind": kind, "enabled": enabled_bool},
        })
        conn.commit()

    resp = RedirectResponse(url=f"/ui/destinations/{did}", status_code=303)
    set_flash(resp, f"Destination {name!r} saved.")
    return resp


# ---- Delete ------------------------------------------------------------


@router.post("/{did}/delete", response_class=Response)
async def delete_destination(
    did: int,
    request: Request,
    auth: Annotated[AuthContext, Depends(ui_auth_required)],
):
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute("SELECT name FROM destinations WHERE id = %s", (did,))
        prior = cur.fetchone()
        if not prior:
            raise HTTPException(404, "destination not found")
        try:
            cur.execute("DELETE FROM destinations WHERE id = %s", (did,))
        except Exception as e:
            conn.rollback()
            # FK violation: still referenced by rule_destinations.
            resp = RedirectResponse(url=f"/ui/destinations/{did}", status_code=303)
            set_flash(resp,
                f"Cannot delete: {e}. Unbind from rules first.",
                "err")
            return resp
        _audit(cur, auth, "destination.delete", did, {"before": {"name": prior["name"]}})
        conn.commit()

    resp = RedirectResponse(url="/ui/destinations", status_code=303)
    set_flash(resp, f"Destination {prior['name']!r} deleted.")
    return resp


# ---- Helpers -----------------------------------------------------------


def _empty_destination(kind: str = "dicom") -> dict:
    return {"id": None, "name": "", "description": "", "kind": kind,
            "enabled": True,
            "config": KIND_HELP.get(kind, ("", "{}"))[1],
            "credential_id": None,
            "dispatch_concurrency": 4,
            "retry_policy": DEFAULT_RETRY_POLICY}


def _load_credentials() -> list[dict]:
    with pool().connection() as conn, conn.cursor() as cur:
        cur.execute(
            "SELECT id, name, kind FROM credentials ORDER BY name"
        )
        return list(cur.fetchall())


def _validate_destination_form(*, name, kind, config, retry_policy,
                                dispatch_concurrency):
    errors: dict[str, str] = {}
    config_obj: dict = {}
    retry_obj: dict | None = None

    if not name or len(name) > 200:
        errors["name"] = "Name must be 1–200 characters."
    if kind not in KIND_HELP:
        errors["kind"] = f"Unknown kind {kind!r}."
    if not (1 <= dispatch_concurrency <= 64):
        errors["dispatch_concurrency"] = "Concurrency must be between 1 and 64."

    if not config.strip():
        errors["config"] = "Config cannot be empty."
    else:
        try:
            config_obj = json.loads(config)
            if not isinstance(config_obj, dict):
                errors["config"] = "Config must be a JSON object (got " + type(config_obj).__name__ + ")."
        except json.JSONDecodeError as e:
            errors["config"] = f"Invalid JSON: {e.msg} at line {e.lineno}, col {e.colno}"

    if retry_policy.strip():
        try:
            retry_obj = json.loads(retry_policy)
            if not isinstance(retry_obj, dict):
                errors["retry_policy"] = "Retry policy must be a JSON object."
        except json.JSONDecodeError as e:
            errors["retry_policy"] = f"Invalid JSON: {e.msg} at line {e.lineno}, col {e.colno}"

    return errors, config_obj, retry_obj


def _audit(cur, auth: AuthContext, action: str, did: int, diff: dict) -> None:
    cur.execute("""
        INSERT INTO admin_audit (actor, actor_kind, action,
                                 resource_kind, resource_id, diff)
        VALUES (%s, %s, %s, %s, %s, %s::jsonb)
    """, (str(auth), "token", action, "destination", str(did), json.dumps(diff)))
