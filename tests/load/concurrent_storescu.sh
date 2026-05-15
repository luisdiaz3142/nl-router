#!/usr/bin/env bash
# tests/load/concurrent_storescu.sh — fan out N storescu jobs in parallel.
#
# Tests the receiver's DcmSCPPool admission control: jobs above
# max_associations should get A-ASSOCIATE-RJ "Local Limit Exceeded";
# jobs at-or-below should all succeed.
#
# Usage:
#   N=24 PORT=11112 ./concurrent_storescu.sh /tmp/nlr-test.dcm
#
# Reports: how many succeeded, how many were rejected by limit, and how
# many failed for some other reason. Writes per-job logs to /tmp/scu-*.log
# (overwritten each run).

set -u

N=${N:-24}
PORT=${PORT:-11112}
HOST=${HOST:-localhost}
AEC=${AEC:-NL_ROUTER}
DCM_FILE=${1:-/tmp/nlr-test.dcm}

rm -f /tmp/scu-*.log /tmp/scu-*.rc

start=$(date +%s)

for i in $(seq 1 "${N}"); do
    (
        storescu -aec "${AEC}" -aet "TEST${i}" \
            "${HOST}" "${PORT}" "${DCM_FILE}" >/tmp/scu-${i}.log 2>&1
        echo $? >/tmp/scu-${i}.rc
    ) &
done

wait
end=$(date +%s)
wall=$(( end - start ))

succeeded=0
rejected_limit=0
rejected_other=0
errors=0

for i in $(seq 1 "${N}"); do
    rc=$(cat /tmp/scu-${i}.rc 2>/dev/null || echo 999)
    log=$(cat /tmp/scu-${i}.log 2>/dev/null)

    if [ "${rc}" = "0" ]; then
        succeeded=$((succeeded + 1))
    elif echo "${log}" | grep -q "Local Limit Exceeded"; then
        rejected_limit=$((rejected_limit + 1))
    elif echo "${log}" | grep -q "Rejected"; then
        rejected_other=$((rejected_other + 1))
    else
        errors=$((errors + 1))
    fi
done

printf "concurrent_storescu N=%d wall=%ds\n" "${N}" "${wall}"
printf "  succeeded:               %3d\n" "${succeeded}"
printf "  rejected (local limit):  %3d\n" "${rejected_limit}"
printf "  rejected (other reason): %3d\n" "${rejected_other}"
printf "  errors:                  %3d\n" "${errors}"
