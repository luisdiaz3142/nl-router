#!/usr/bin/env bash
# tests/security/phi_leak_grep.sh — verify no PHI leaks into logs.
#
# Design-plan checkpoint: "No PHI appears in any log at any level
# (grep test against patient_name in test fixtures)."
#
# Strategy: run the test fixture through the receiver, capture the
# structured log stream, and grep for known PHI values from the
# fixture. The receiver MUST NOT emit any of these strings; if any
# match, the test fails.
#
# Run against a live receiver pointing at /tmp/nlr-test.dcm with these
# known-PHI tag values:
#   PatientName            "Test^Patient^E2E"
#   PatientID              "PAT-E2E-12345"
#   ReferringPhysicianName "Refer^Physician"
#   InstitutionName        "Test Hospital"
#
# UIDs and AETs ARE allowed in logs per the design plan (they're not
# PHI under HIPAA's Safe Harbor de-identification list). The receiver
# already logs StudyInstanceUID and calling/called AETs — that's not
# a leak.
#
# Usage:
#   PORT=11112 LOG_FILE=/tmp/nlr-receiver.log ./phi_leak_grep.sh
#
# Returns 0 if clean, 1 if any PHI value found in the log.

set -u

LOG_FILE=${LOG_FILE:-/tmp/nlr-receiver.log}
PORT=${PORT:-11112}
DCM=${DCM:-/tmp/nlr-test.dcm}

if [ ! -f "${DCM}" ]; then
    echo "FATAL: test DICOM ${DCM} missing" 1>&2
    exit 2
fi
if [ ! -f "${LOG_FILE}" ]; then
    echo "FATAL: log file ${LOG_FILE} missing" 1>&2
    exit 2
fi

# The PHI strings we expect to find in the test fixture's DICOM tags
# but NOT to appear anywhere in the receiver's log output. Patterns
# are literal substrings (grep -F).
PHI_STRINGS=(
    "Test^Patient^E2E"
    "Test Patient E2E"
    "PAT-E2E-12345"
    "Refer^Physician"
    "Refer Physician"
    "Test Hospital"
)

echo "==> scanning ${LOG_FILE} for PHI"
fail=0
for phi in "${PHI_STRINGS[@]}"; do
    if grep -F -q -- "${phi}" "${LOG_FILE}"; then
        echo "  [FAIL] log contains PHI: ${phi}"
        grep -F -n -- "${phi}" "${LOG_FILE}" | head -3 | sed 's/^/    /'
        fail=1
    else
        echo "  [ok]   no leak: ${phi}"
    fi
done

if [ ${fail} -eq 1 ]; then
    echo ""
    echo "FAIL: log contains PHI. Logs must not include patient identifiers."
    exit 1
fi

echo ""
echo "PASS: no PHI strings found in ${LOG_FILE}"
exit 0
