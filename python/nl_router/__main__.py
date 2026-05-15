"""Module entry point so `python -m nl_router` works alongside the installed
`nl-router` console script."""

from __future__ import annotations

from nl_router.cli import app


if __name__ == "__main__":
    app()
