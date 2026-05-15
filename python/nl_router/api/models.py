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
# Health
# ============================================================


class HealthOut(BaseModel):
    status: Literal["ok", "degraded"]
    schema_version: int | None
    server_id: str | None
