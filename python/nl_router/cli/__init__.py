"""Top-level Typer app for the `nl-router` CLI.

Each subcommand lives in its own module under this package. We assemble the
app here and register subcommand trees in alphabetical order to keep
`nl-router --help` output stable.
"""

from __future__ import annotations

import typer

from nl_router import __version__
from nl_router.cli import config_io, destination, init, migrate, rule, serve, status

app = typer.Typer(
    name="nl-router",
    help="DICOM router — management CLI.\n\n"
         "Talks to the central Postgres for configuration and audit. "
         "The DICOM hot path runs as separate native processes "
         "(nl-receiver / nl-route / nl-dispatch / nl-clean) and is not "
         "controlled directly from this CLI.",
    no_args_is_help=True,
    add_completion=False,
    pretty_exceptions_show_locals=False,
)


def _version_callback(show: bool) -> None:
    if show:
        typer.echo(f"nl-router {__version__}")
        raise typer.Exit()


@app.callback()
def _root(
    version: bool = typer.Option(
        False,
        "--version",
        callback=_version_callback,
        is_eager=True,
        help="Print version and exit.",
    ),
) -> None:
    """Root callback. Use `nl-router <subcommand> --help` for command-specific help."""
    # No-op; the callback exists so --version works at the root level.
    return


# ---- Subcommand registration ---------------------------------------------
# Keep alphabetical so help output is stable.

app.command(name="config-export")(config_io.export_)
app.command(name="config-import")(config_io.import_)
app.add_typer(destination.app, name="destination", help="Manage outbound destinations.")
app.command(name="init")(init.init)
app.command(name="migrate")(migrate.migrate)
app.add_typer(rule.app, name="rule", help="Manage routing rules.")
app.command(name="serve")(serve.serve)
app.command(name="status")(status.status)


__all__ = ["app"]
