#!/bin/sh
# entrypoint.sh — role dispatcher for the nl-router runtime image.
#
# The single image ships every binary in the project. This script picks
# which one to exec based on the first argument, so deployments only need
# to set `command: [<role>]` in their compose/Kubernetes manifest.
#
# Conventions:
#   * Bare roles (receiver, route, dispatcher, cleaner, api) exec the
#     corresponding binary, forwarding additional args.
#   * `module <kind>` runs the processing module at
#     /usr/libexec/nl-router/modules/<kind>.
#   * `cli` is a generic passthrough to /usr/bin/nl-router, useful for
#     `docker exec` into a running pod (`docker exec foo cli destination list`).
#   * `help` (or no arg) prints this usage.

set -eu

usage() {
    cat <<'EOF'
nl-router runtime image. Pass a role as the first argument:

  receiver               run nl-receiver (DICOM SCP)
  route                  run nl-route (rule evaluator)
  dispatcher             run nl-dispatch (outbound sender)
  cleaner                run nl-clean (file + row retention)
  api                    run the FastAPI management API
  module <kind>          run a processing module (e.g. anonymize_basic)
  migrate <args>         run nl-router migrate (up | down N | version | force <V>)
  init <args>            run nl-router init (first-run wizard)
  cli <args>             generic passthrough to nl-router CLI

Examples:
  docker run -e NL_ROUTER_DATABASE_URL=postgres://... nl-router receiver
  docker run nl-router migrate up
  docker run nl-router cli rule list
EOF
}

if [ $# -eq 0 ]; then
    usage
    exit 0
fi

ROLE="$1"
shift

case "${ROLE}" in
    receiver)    exec /usr/bin/nl-receiver "$@" ;;
    route)       exec /usr/bin/nl-route "$@" ;;
    dispatcher)  exec /usr/bin/nl-dispatch "$@" ;;
    cleaner)     exec /usr/bin/nl-clean "$@" ;;
    api)         exec /usr/bin/nl-router serve "$@" ;;
    migrate)     exec /usr/bin/nl-router migrate "$@" ;;
    init)        exec /usr/bin/nl-router init "$@" ;;
    cli)         exec /usr/bin/nl-router "$@" ;;
    module)
        if [ $# -eq 0 ]; then
            echo "entrypoint: 'module' requires a kind (e.g. anonymize_basic)" >&2
            exit 2
        fi
        KIND="$1"
        shift
        BIN="/usr/libexec/nl-router/modules/${KIND}"
        if [ ! -x "${BIN}" ]; then
            echo "entrypoint: no module binary at ${BIN}" >&2
            exit 127
        fi
        # The worker contract requires NL_ROUTER_MODULE_KIND to match the
        # processing_modules.kind column. We set it here so operators don't
        # have to remember to pass it via -e.
        export NL_ROUTER_MODULE_KIND="${KIND}"
        exec "${BIN}" "$@"
        ;;
    help|--help|-h) usage ;;
    *)
        echo "entrypoint: unknown role: ${ROLE}" >&2
        echo "Run with 'help' for usage." >&2
        exit 2
        ;;
esac
