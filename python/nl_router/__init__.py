"""nl-router — DICOM router with centralized configuration.

This package provides:
    * The management CLI (`nl-router` command, Typer-based).
    * The FastAPI management API + UI (under the `api` optional extra).
    * Shared library code for talking to the central Postgres.

The DICOM hot path (receiver / router / dispatcher / cleaner / processing
module workers) is implemented as separate native C++ binaries that share
this Postgres database.
"""

from __future__ import annotations

__version__ = "0.0.1"

__all__ = ["__version__"]
