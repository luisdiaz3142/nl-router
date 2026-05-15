"""nl-router management API (FastAPI).

This is the operator-facing HTTP surface for managing rules, destinations,
credentials, and watching traffic. The DICOM hot path (receiver / router /
dispatcher / cleaner) runs as separate native binaries; this API never
touches DICOM directly — it only reads and writes the same Postgres
database those processes share.

Routes mount under `/api/v1/...`. `/healthz` is the standard health probe
and is unauthenticated.
"""

from __future__ import annotations
