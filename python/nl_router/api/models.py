"""Pydantic request/response models for the management API.

We deliberately keep these flat and JSON-shaped — they're the operator-
facing wire format and should mirror the DB columns as closely as
possible, not introduce a separate domain layer.
"""

from __future__ import annotations

from datetime import datetime
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field


# ============================================================
# Rules
# ============================================================


class RuleBase(BaseModel):
    """Shared fields for create/update."""

    model_config = ConfigDict(extra="forbid")

    name: str = Field(..., min_length=1, max_length=200,
                      description="Unique human-readable rule name.")
    description: str | None = Field(default=None)
    scope: Literal["study", "series"] = Field(default="study")
    predicate: str = Field(..., min_length=1,
                           description="dicomdiablo-compatible expression DSL.")
    priority: int = Field(default=0, ge=-32768, le=32767)
    status: Literal["draft", "disabled", "enabled"] = Field(default="draft")
    dispatch_order: Literal["parallel", "sequential"] = Field(default="parallel")


class RuleCreate(RuleBase):
    pass


class RuleUpdate(BaseModel):
    """Partial update — every field optional. We don't allow renaming via
    PATCH; that would require careful uniqueness handling. Operators can
    delete + recreate if they need a different name."""

    model_config = ConfigDict(extra="forbid")

    description: str | None = None
    scope: Literal["study", "series"] | None = None
    predicate: str | None = None
    priority: int | None = None
    status: Literal["draft", "disabled", "enabled"] | None = None
    dispatch_order: Literal["parallel", "sequential"] | None = None


class RuleOut(RuleBase):
    id: int
    created_at: datetime
    created_by: str | None
    updated_at: datetime
    updated_by: str | None


# ============================================================
# Destinations
# ============================================================


class DestinationBase(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str = Field(..., min_length=1, max_length=200)
    description: str | None = None
    # `kind` is TEXT in the DB so custom kinds can be added without schema
    # changes. v1 ships only `dicom` as a built-in dispatcher, but the API
    # accepts any kind string — operator-supplied workers handle custom kinds.
    kind: str = Field(..., min_length=1, max_length=50)
    enabled: bool = True
    config: dict[str, Any] = Field(default_factory=dict,
                                    description="kind-specific config JSONB")
    dispatch_concurrency: int = Field(default=4, ge=1, le=128)
    credential_id: int | None = None
    retry_policy: dict[str, Any] | None = Field(
        default=None,
        description=(
            "Optional retry policy override; falls back to DB default "
            "({max_attempts, initial_backoff_s, multiplier, max_backoff_s, "
            "give_up_after_hours}) if omitted."
        ),
    )


class DestinationCreate(DestinationBase):
    pass


class DestinationUpdate(BaseModel):
    model_config = ConfigDict(extra="forbid")

    description: str | None = None
    kind: str | None = None
    enabled: bool | None = None
    config: dict[str, Any] | None = None
    dispatch_concurrency: int | None = Field(default=None, ge=1, le=128)
    credential_id: int | None = None
    retry_policy: dict[str, Any] | None = None


class DestinationOut(DestinationBase):
    id: int
    created_at: datetime
    updated_at: datetime


# ============================================================
# Rule ↔ destination bindings
# ============================================================


class RuleDestinationBind(BaseModel):
    """Body for binding a rule to a destination (or updating the binding)."""

    model_config = ConfigDict(extra="forbid")

    ordinal: int = Field(default=0, ge=-32768, le=32767,
                          description="Order among the rule's destinations. "
                                       "Only meaningful when rules.dispatch_order='sequential'.")
    retry_policy_override: dict[str, Any] | None = Field(
        default=None,
        description="Optional per-pairing override of the destination's retry_policy.",
    )


class RuleDestinationOut(BaseModel):
    id: int
    rule_id: int
    destination_id: int
    destination_name: str
    destination_kind: str
    ordinal: int
    retry_policy_override: dict[str, Any] | None


# ============================================================
# work_queue (read-only)
# ============================================================


class WorkQueueSummary(BaseModel):
    """Lightweight row for list responses. The full JSONB tags live on
    WorkQueueDetail to keep the list payload bounded."""

    id: int
    server_id: str
    status: str
    study_instance_uid: str
    series_instance_uid: str | None
    accession_number: str | None
    patient_id: str | None
    patient_name: str | None
    modality: str | None
    station_name: str | None
    study_description: str | None
    calling_aet: str
    called_aet: str
    instance_count: int
    byte_count: int
    received_at: datetime
    closed_at: datetime
    routed_at: datetime | None
    dispatched_at: datetime | None
    cleaned_at: datetime | None
    close_trigger: str
    priority: int
    retry_count: int
    failed_phase: str | None
    last_error: str | None
    cleanup_hold: bool


class WorkQueueDetail(WorkQueueSummary):
    """Full row including the JSONB tags blob and on-disk path."""

    series_description: str | None
    protocol_name: str | None
    body_part_examined: str | None
    referring_physician_name: str | None
    study_date: Any | None     # `date` would force isoformat coercion; let
    study_time: Any | None     #  Pydantic accept whatever psycopg returns
    series_date: Any | None
    series_time: Any | None
    acquisition_date: Any | None
    acquisition_time: Any | None
    institution_name: str | None
    manufacturer: str | None
    manufacturer_model_name: str | None
    file_root_path: str
    claimed_by: str | None
    claimed_at: datetime | None
    claim_expires_at: datetime | None
    cleanup_hold_reason: str | None
    cleanup_hold_by: str | None
    cleanup_hold_at: datetime | None
    next_retry_at: datetime | None
    tags: dict[str, Any]


class PageInfo(BaseModel):
    """Pagination envelope used by list endpoints. Offset-based for now;
    cursor-based pagination is a v2 nice-to-have once row volumes grow."""

    total: int
    limit: int
    offset: int


class WorkQueuePage(BaseModel):
    items: list[WorkQueueSummary]
    page: PageInfo


# ============================================================
# route_assignments (read-only)
# ============================================================


class AssignmentOut(BaseModel):
    id: int
    work_queue_id: int
    rule_id: int
    destination_id: int
    dispatch_kind: str
    server_id: str
    status: str
    attempts: int
    last_error: str | None
    next_retry_at: datetime | None
    dispatched_at: datetime | None
    claimed_by: str | None
    claimed_at: datetime | None
    claim_expires_at: datetime | None
    response_detail: dict[str, Any] | None
    created_at: datetime


class AssignmentPage(BaseModel):
    items: list[AssignmentOut]
    page: PageInfo


# ============================================================
# admin_audit (read-only)
# ============================================================


class AuditOut(BaseModel):
    id: int
    actor: str
    actor_kind: str
    action: str
    resource_kind: str | None
    resource_id: str | None
    diff: dict[str, Any] | None
    client_ip: str | None
    user_agent: str | None
    occurred_at: datetime


class AuditPage(BaseModel):
    items: list[AuditOut]
    page: PageInfo


# ============================================================
# API tokens
# ============================================================


class TokenCreate(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str = Field(..., min_length=1, max_length=200)
    role: Literal["admin", "operator", "viewer", "service"] = Field(
        default="operator",
        description="Permission bundle. 'service' starts with an empty "
                     "permission set; pass `permissions` to scope it.",
    )
    permissions: list[str] | None = Field(
        default=None,
        description="Override the role's default permissions with an explicit set.",
    )
    expires_at: datetime | None = None


class TokenCreatedOut(BaseModel):
    """Response shape for token creation. Returns the raw token exactly
    once — there is no other path to retrieve it."""

    id: int
    name: str
    permissions: list[str]
    expires_at: datetime | None
    created_at: datetime
    raw_token: str = Field(
        ...,
        description="The actual token. Shown once; not retrievable later.",
    )


class TokenOut(BaseModel):
    """List/get representation. Never includes the raw token or its hash."""

    id: int
    name: str
    permissions: list[str]
    revoked: bool
    expires_at: datetime | None
    last_used_at: datetime | None
    created_at: datetime
    created_by: int | None


# ============================================================
# Health
# ============================================================


class HealthOut(BaseModel):
    status: Literal["ok", "degraded"]
    schema_version: int | None
    server_id: str | None
