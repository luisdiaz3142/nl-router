"""Per-kind credential payload schemas.

Each credential `kind` declares a Pydantic model for its plaintext payload.
The CRUD route validates the incoming JSON against the kind-specific model
before encryption, so malformed payloads fail at the boundary with a 422
instead of getting silently encrypted as opaque bytes.

Adding a new kind:
    1. Define a CredentialPayload* model below.
    2. Add it to `_KIND_MODELS`.
    3. The route picks it up automatically.

The `gcp_service_account` kind takes the full JSON blob downloaded from
GCP. We don't model every field strictly (Google can add new ones); we
validate the load-bearing ones and let the rest pass through.
"""

from __future__ import annotations

from typing import Any, Final

from pydantic import BaseModel, ConfigDict, Field, ValidationError


class _Strict(BaseModel):
    """Base for credential payloads. Strict by default so typos surface."""

    model_config = ConfigDict(extra="forbid")


class CredentialPayloadBasicHttp(_Strict):
    username: str = Field(..., min_length=1)
    password: str = Field(..., min_length=1)


class CredentialPayloadBearerToken(_Strict):
    token: str = Field(..., min_length=1)


class CredentialPayloadApiKey(_Strict):
    """Allows EITHER header-based or query-param-based API keys.

    Exactly one of `header` / `query_param` must be set."""

    header: str | None = None
    query_param: str | None = None
    value: str = Field(..., min_length=1)

    def model_post_init(self, _ctx: Any) -> None:
        if (self.header is None) == (self.query_param is None):
            raise ValueError("api_key payload requires exactly one of 'header' or 'query_param'")


class CredentialPayloadAwsKeys(_Strict):
    access_key_id: str = Field(..., min_length=1)
    secret_access_key: str = Field(..., min_length=1)
    session_token: str | None = None


class CredentialPayloadGcpServiceAccount(BaseModel):
    """Loose validation for the GCP service-account JSON file.

    We require the fields the OAuth2 token-exchange needs and let Google's
    extra fields pass through. Most operators paste in the full JSON
    verbatim."""

    model_config = ConfigDict(extra="allow")

    type: str = Field(..., description="must be 'service_account'")
    client_email: str = Field(..., min_length=1)
    private_key: str = Field(..., min_length=1)
    private_key_id: str | None = None
    project_id: str | None = None
    token_uri: str | None = None
    client_id: str | None = None

    def model_post_init(self, _ctx: Any) -> None:
        if self.type != "service_account":
            raise ValueError(
                f"gcp_service_account: 'type' must be 'service_account', got {self.type!r}"
            )


class CredentialPayloadMtlsCert(_Strict):
    cert_pem: str = Field(..., min_length=1)
    key_pem:  str = Field(..., min_length=1)
    ca_pem:   str | None = None


class CredentialPayloadTlsCert(_Strict):
    cert_pem: str = Field(..., min_length=1)
    key_pem:  str = Field(..., min_length=1)


_KIND_MODELS: Final[dict[str, type[BaseModel]]] = {
    "basic_http":           CredentialPayloadBasicHttp,
    "bearer_token":         CredentialPayloadBearerToken,
    "api_key":              CredentialPayloadApiKey,
    "aws_keys":             CredentialPayloadAwsKeys,
    "gcp_service_account":  CredentialPayloadGcpServiceAccount,
    "mtls_cert":            CredentialPayloadMtlsCert,
    "tls_cert":             CredentialPayloadTlsCert,
}


def known_kinds() -> list[str]:
    """Sorted list of all credential kinds this version understands."""
    return sorted(_KIND_MODELS)


def validate_payload(kind: str, raw: dict[str, Any]) -> dict[str, Any]:
    """Validate a raw plaintext payload against its kind's model.

    Returns the validated payload (with defaults applied, extras pruned for
    strict kinds). Raises pydantic.ValidationError on failure — the FastAPI
    layer turns that into a 422.
    """
    model_cls = _KIND_MODELS.get(kind)
    if model_cls is None:
        raise ValueError(
            f"unknown credential kind: {kind!r}. "
            f"Known kinds: {', '.join(known_kinds())}"
        )
    parsed = model_cls.model_validate(raw)
    return parsed.model_dump(exclude_none=False)
