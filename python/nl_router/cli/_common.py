"""Shared helpers for the CLI: error reporting, table rendering, formatters."""

from __future__ import annotations

import sys
from typing import Any, Iterable, NoReturn

import typer
from rich.console import Console
from rich.table import Table

# stdout for normal output; stderr for diagnostics. Tests inspect both.
out = Console()
err = Console(stderr=True)


def die(msg: str, *, code: int = 1) -> NoReturn:
    """Print an error message to stderr and exit non-zero.

    Used by command implementations for expected-failure paths (missing
    resources, validation errors, etc.). Unexpected exceptions are left to
    Typer's normal traceback handling.
    """
    err.print(f"[bold red]error:[/bold red] {msg}")
    raise typer.Exit(code=code)


def render_table(
    title: str | None,
    columns: list[str],
    rows: Iterable[Iterable[Any]],
    *,
    empty_message: str = "(no rows)",
) -> None:
    """Render a tabular result. If no rows, print empty_message instead.

    Booleans render as ✓ / ✗, NULLs render as a dim dash, everything else
    as `str(value)`.
    """
    rendered_rows = [list(r) for r in rows]
    if not rendered_rows:
        out.print(f"[dim]{empty_message}[/dim]")
        return

    table = Table(title=title, show_header=True, header_style="bold cyan")
    for col in columns:
        table.add_column(col)
    for row in rendered_rows:
        table.add_row(*(_fmt_cell(c) for c in row))
    out.print(table)


def _fmt_cell(value: Any) -> str:
    if value is None:
        return "[dim]—[/dim]"
    if isinstance(value, bool):
        return "[green]✓[/green]" if value else "[red]✗[/red]"
    return str(value)
