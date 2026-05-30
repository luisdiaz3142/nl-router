"""DSL predicate validation — shared helper used by the API model layer
and the `nl-router validate-predicate` CLI subcommand.

We don't reimplement the parser in Python; we shell out to the
`nl-dsl-validate` C++ helper binary built from cpp/common/dsl/tools/.
That keeps the grammar single-source — anything the router parses is
exactly what this validates — and the binary is ~1 ms per call.

Public surface:
    validate_predicate(text) -> ValidateResult

`ValidateResult.ok` is True on a clean parse, False otherwise; on
failure `detail` carries the parser's own line/column message (the
form operators want to see). `binary_available` distinguishes the
"all good" case from the "no binary installed, parse skipped" case
that the API layer treats as a soft pass for dev mode.

Discovery order for the binary:
    1. $NL_ROUTER_DSL_VALIDATE_BIN env override (dev mode)
    2. /usr/libexec/nl-router/nl-dsl-validate (canonical .deb path)
    3. shutil.which("nl-dsl-validate") (PATH fallback)

Pre-M27 the shellout lived inline in nl_router.api.models. M27
factored it out so the CLI subcommand could call the same code
without re-implementing the discovery + framing logic.
"""

from __future__ import annotations

import os
import shutil
import subprocess
from dataclasses import dataclass


CANONICAL_BINARY_PATH = "/usr/libexec/nl-router/nl-dsl-validate"
ENV_OVERRIDE = "NL_ROUTER_DSL_VALIDATE_BIN"
DEFAULT_TIMEOUT_S = 5.0


@dataclass(frozen=True)
class ValidateResult:
    """Outcome of one validate_predicate() call.

    Fields:
      ok                — True on clean parse OR (no binary + ok-on-missing-true).
      detail            — parser stderr verbatim on failure; "" otherwise.
      binary_available  — True if we found nl-dsl-validate, False if not.
                          Callers that strictly require a real parse check
                          (e.g. the CLI) use this to decide whether to
                          warn the operator.
    """

    ok: bool
    detail: str
    binary_available: bool


def find_binary() -> str | None:
    """Locate the nl-dsl-validate binary on this host.

    Returns the absolute path, or None if no copy is installed. The
    env override is honored first so developers can point at a
    cpp/build/... copy without sudo-installing the .deb.
    """
    override = os.environ.get(ENV_OVERRIDE)
    if override:
        return override if os.path.isfile(override) else None
    if os.path.isfile(CANONICAL_BINARY_PATH):
        return CANONICAL_BINARY_PATH
    return shutil.which("nl-dsl-validate")


def validate_predicate(
    text: str,
    *,
    timeout_s: float = DEFAULT_TIMEOUT_S,
) -> ValidateResult:
    """Run nl-dsl-validate against `text`.

    Empty input is a parse error (matches the helper's behavior). The
    `binary_available` field in the result lets the caller distinguish
    "valid" from "we couldn't actually check" — the API layer accepts
    the latter as a soft pass for dev mode, but the CLI surfaces it
    as a warning so operators aren't fooled.
    """
    binary = find_binary()
    if binary is None:
        return ValidateResult(
            ok=True,                # soft-pass; API layer behavior pre-M22
            detail="",
            binary_available=False,
        )

    try:
        proc = subprocess.run(
            [binary],
            input=text.encode("utf-8"),
            capture_output=True,
            timeout=timeout_s,
        )
    except subprocess.TimeoutExpired:
        return ValidateResult(
            ok=False,
            detail="predicate validation timed out",
            binary_available=True,
        )

    if proc.returncode == 0:
        return ValidateResult(ok=True, detail="", binary_available=True)

    # The helper writes one-line stderr messages with line/column info
    # ("syntax error (at line N, column M)") — exactly what an operator
    # wants to see. Pass it through unmodified.
    msg = proc.stderr.decode("utf-8", errors="replace").strip()
    return ValidateResult(
        ok=False,
        detail=msg or "predicate failed to parse (no detail)",
        binary_available=True,
    )
