"""nl-router operator UI.

Server-rendered (Jinja2) + HTMX. Mounted at /ui under the same FastAPI
app that serves the REST API at /api/v1, so they share the database
pool, auth surface, and lifespan.

Auth model: HTML routes look at the nlr_session cookie (HttpOnly,
SameSite=Strict), which carries a raw API token issued by
`nl-router init` or `POST /api/v1/tokens`. The cookie value is
validated against the api_tokens table the same way the
Authorization header is — see auth._validate_raw_token. Tokens that
work for /api/v1 work for /ui, no separate user table.

Local users with Argon2id passwords and OIDC SSO are tracked in the
deferred list; the token-paste login keeps the auth contract simple
and ships now without adding new schema.
"""
