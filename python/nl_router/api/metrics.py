"""Prometheus metrics for the management API process.

Catalog (mirrors the per-daemon /metrics surfaces in cpp/):

    nl_api_requests_total{method, route, status}              counter
    nl_api_request_duration_seconds{method, route}            histogram
    nl_api_auth_failures_total{reason}                        counter
        reason ∈ {missing_header, malformed_header, invalid_token,
                  permission_denied}
    nl_api_audit_events_total{action}                         counter
        action carries the admin_audit.action value
        (e.g. rule.create, destination.delete, hold.set)
    nl_api_db_pool_size                                       gauge
    nl_api_db_pool_in_use                                     gauge

The HTTP exposer runs on its own port (BootstrapConfig.metrics_port,
default 9184) via prometheus_client.start_http_server — a tiny daemon
thread, separate from the main FastAPI/Uvicorn server. Operators scrape
the metrics port directly; nothing here lives behind auth.

Route labeling: we use the FastAPI route's path template (e.g.
``/api/v1/rules/{id}``) rather than the literal path. This bounds
cardinality — a request bursting at one rule id won't blow up the
counter — and keeps the metric meaningful across rule churn.
"""

from __future__ import annotations

import logging
import time
from typing import Awaitable, Callable

from prometheus_client import (
    Counter,
    Gauge,
    Histogram,
    start_http_server,
)
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import Response

log = logging.getLogger("nl_router.api.metrics")


# ---- catalog -------------------------------------------------------------

# Request duration: API calls dominated by single Postgres round-trips
# under steady state. A 5s tail covers the migrate / config-export / heavy
# admin-audit JOIN edge cases without going overboard on bucket count.
_REQUEST_BUCKETS = (
    0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0,
)

REQUESTS_TOTAL = Counter(
    "nl_api_requests_total",
    "HTTP requests served by the management API, by method/route/status",
    ["method", "route", "status"],
)

REQUEST_DURATION = Histogram(
    "nl_api_request_duration_seconds",
    "HTTP request handler wall-clock duration",
    ["method", "route"],
    buckets=_REQUEST_BUCKETS,
)

AUTH_FAILURES_TOTAL = Counter(
    "nl_api_auth_failures_total",
    "Authentication/authorization failures, by reason",
    ["reason"],
)

AUDIT_EVENTS_TOTAL = Counter(
    "nl_api_audit_events_total",
    "admin_audit rows emitted, by action",
    ["action"],
)

DB_POOL_SIZE = Gauge(
    "nl_api_db_pool_size",
    "Total psycopg connection pool size (open connections)",
)

DB_POOL_IN_USE = Gauge(
    "nl_api_db_pool_in_use",
    "psycopg connections currently checked out of the pool",
)


# ---- request middleware --------------------------------------------------

class PrometheusMiddleware(BaseHTTPMiddleware):
    """Time + count every request by route template + method + status.

    We resolve the matched route via ``request.scope["route"]``; if the
    request didn't match any route (e.g. 404), we record the raw path
    bucketed as ``__unmatched__`` so cardinality stays bounded.

    The /metrics endpoint runs on a separate port (see ``serve_metrics``),
    so requests to the FastAPI app itself never include scrapes — no
    self-counting bias.
    """

    async def dispatch(
        self,
        request: Request,
        call_next: Callable[[Request], Awaitable[Response]],
    ) -> Response:
        start = time.perf_counter()
        # Default response in case of an uncaught exception below; we
        # still want to record the request as a 500.
        status_code = 500
        try:
            response = await call_next(request)
            status_code = response.status_code
            return response
        finally:
            elapsed = time.perf_counter() - start
            route_template = _route_label(request)
            method = request.method
            REQUEST_DURATION.labels(method=method, route=route_template).observe(elapsed)
            REQUESTS_TOTAL.labels(
                method=method,
                route=route_template,
                status=str(status_code),
            ).inc()


def _route_label(request: Request) -> str:
    """Resolve the matched route's path template, falling back to a bucket."""
    route = request.scope.get("route")
    # Starlette's Route has a `path` attribute; mounted apps may have
    # something else (e.g. StaticFiles). Be defensive.
    template = getattr(route, "path", None)
    if isinstance(template, str) and template:
        return template
    return "__unmatched__"


# ---- exposer -------------------------------------------------------------

def serve_metrics(port: int, addr: str = "0.0.0.0") -> None:
    """Start the prometheus_client HTTP server on ``addr:port``.

    Port 0 disables — useful for tests and for operators who deliberately
    don't want a metrics port open. The exposer runs in a daemon thread;
    no explicit shutdown is required (it dies with the process).
    """
    if port == 0:
        log.info("api.metrics_disabled")
        return
    try:
        start_http_server(port, addr=addr)
        log.info("api.metrics_listening", extra={"addr": addr, "port": port})
    except OSError as e:
        # Don't bring the API down because the metrics port collided;
        # surface a loud log and continue. Operators will see the missing
        # scrape target in their alerting.
        log.error("api.metrics_listen_failed", extra={"addr": addr, "port": port, "error": str(e)})
