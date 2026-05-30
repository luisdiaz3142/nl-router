"""`nl-router validate-predicate` — dry-run a routing-rule predicate.

Operator-facing wrapper over the C++ `nl-dsl-validate` helper binary.
Useful for:

  * Testing a predicate's syntax before saving the rule via the UI or
    API — the validator inside the API rejects bad predicates at save
    time (M22), but operators often want to iterate locally first.
  * Scripting rule deployments — `validate-predicate` in CI before
    pushing a rules-import file gives a clean exit code to gate on.
  * Quickly checking what an unfamiliar grammar feature ("does `in`
    work on tuples?") accepts, without round-tripping through HTTP.

Modes (mutually exclusive):
  positional arg   $ nl-router validate-predicate 'tags.Modality == "CT"'
  stdin            $ echo 'true' | nl-router validate-predicate -
  file             $ nl-router validate-predicate --file rule.txt

Exit codes:
  0   predicate parsed cleanly
  1   parse failure (stderr has the parser's line/column message)
  2   I/O / config issue (no helper installed, timeout, etc.)
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Annotated

import typer

from nl_router import dsl as dsl_validator
from nl_router.cli._common import err, out


def validate_predicate_cmd(
    predicate: Annotated[
        str | None,
        typer.Argument(
            help="Predicate text. Pass '-' to read from stdin instead.",
        ),
    ] = None,
    file: Annotated[
        Path | None,
        typer.Option(
            "--file", "-f",
            help="Read the predicate from this file instead of an argument.",
        ),
    ] = None,
    quiet: Annotated[
        bool,
        typer.Option(
            "--quiet", "-q",
            help="Suppress the ✓ on success; only print on failure.",
        ),
    ] = False,
) -> None:
    """Parse a routing-rule predicate and report whether it's valid.

    The check uses the exact same parser the router applies on cache
    refresh — anything that passes here is guaranteed to load into the
    rule cache. The Python-side structural checks (length, paren
    balance) the API also runs are NOT applied here; this is a
    grammar-only check, on the theory that operators dry-running a
    predicate from the shell already know about size limits.
    """
    text = _resolve_input(predicate, file)
    if text is None:
        err.print(
            "[bold red]error:[/bold red] no predicate provided. "
            "Pass it as an argument, via --file, or via stdin (-).",
        )
        raise typer.Exit(code=2)

    result = dsl_validator.validate_predicate(text)

    if not result.binary_available:
        err.print(
            "[bold yellow]warning:[/bold yellow] "
            "nl-dsl-validate not installed; check skipped. "
            "Build the C++ side or install the .deb to enable a real parse."
        )
        # Soft pass — same semantics as the API layer's dev-mode fallthrough.
        raise typer.Exit(code=2)

    if result.ok:
        if not quiet:
            out.print("[bold green]✓[/bold green] valid")
        raise typer.Exit(code=0)

    err.print(f"[bold red]✗[/bold red] {result.detail}")
    raise typer.Exit(code=1)


def _resolve_input(positional: str | None, file: Path | None) -> str | None:
    """Pick the predicate source — exactly one of positional / stdin / file.

    Returns the text, or None if nothing was provided. Conflicting
    combinations (both arg and --file) take the file's content; that
    matches gnu-style command behavior where explicit flags win.
    """
    if file is not None:
        return file.read_text(encoding="utf-8")
    if positional == "-":
        return sys.stdin.read()
    if positional is not None:
        return positional
    # No explicit source — if stdin is piped (not a TTY), read it.
    # That's friendlier than forcing operators to remember the `-`.
    if not sys.stdin.isatty():
        return sys.stdin.read()
    return None
