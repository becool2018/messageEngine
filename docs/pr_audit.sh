#!/usr/bin/env bash
# docs/pr_audit.sh — Pre-PR documentation audit
#
# Inspects the diff between this branch and main (or a named base branch) and
# decides which documentation checklist items are triggered, then verifies
# whether each triggered item has been satisfied.  Prints a filled-in
# "Documentation updated" block ready to paste into the PR description.
#
# Usage:
#   bash docs/pr_audit.sh [BASE_BRANCH]   # default: main
#   make pr-audit
#
# Exit codes:
#   0 — all REQUIRED items are satisfied (WARN items need human confirmation)
#   1 — one or more REQUIRED items are triggered but not yet satisfied
#
# NSC-infrastructure: build/CI tooling — no REQ-x.x applies.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

BASE="${1:-main}"
MERGE_BASE="$(git merge-base "$BASE" HEAD 2>/dev/null || true)"
if [ -z "$MERGE_BASE" ]; then
    echo "ERROR: cannot determine merge-base against '$BASE'. Is this a git repository?" >&2
    exit 1
fi

# ── Changed file sets ─────────────────────────────────────────────────────────
ALL_CHANGED="$(git diff --name-only "$MERGE_BASE"...HEAD)"
SRC_CHANGED="$(printf '%s\n' "$ALL_CHANGED" | grep '^src/' || true)"
TEST_CHANGED="$(printf '%s\n' "$ALL_CHANGED" | grep '^tests/' || true)"
DOC_CHANGED="$(printf '%s\n' "$ALL_CHANGED" | grep '^docs/' || true)"

# Source files that carry a Safety-critical (SC): annotation
SC_FILES_CHANGED="$(for f in $SRC_CHANGED; do
    [ -f "$f" ] && grep -lq 'Safety-critical (SC):' "$f" 2>/dev/null && echo "$f"
done || true)"

# State-machine component files (AckTracker, RetryManager, ImpairmentEngine,
# ReassemblyBuffer, OrderingBuffer — see DOC_MAINTENANCE.md §2.3)
SM_FILES_CHANGED="$(printf '%s\n' "$SRC_CHANGED" \
    | grep -E 'AckTracker|RetryManager|ImpairmentEngine|ReassemblyBuffer|OrderingBuffer' \
    || true)"

# Capacity-constant file (Types.hpp — see DOC_MAINTENANCE.md §2.1)
TYPES_CHANGED="$(printf '%s\n' "$ALL_CHANGED" | grep 'src/core/Types\.hpp' || true)"

# Loop-algorithm files (trigger stress tests alongside Types.hpp changes)
LOOP_FILES_CHANGED="$(printf '%s\n' "$SRC_CHANGED" \
    | grep -E 'AckTracker|RetryManager|RingBuffer|DuplicateFilter' \
    || true)"

# Non-trivial src/ changes: any .cpp or .hpp under src/
NONTRIVIAL_SRC="$(printf '%s\n' "$SRC_CHANGED" | grep -E '\.(cpp|hpp)$' || true)"

# Helper: was a specific doc path modified in this branch?
doc_updated() { printf '%s\n' "$ALL_CHANGED" | grep -qx "$1" && return 0 || return 1; }

# ── Audit state ───────────────────────────────────────────────────────────────
OVERALL_FAIL=0   # set to 1 on any REQUIRED+unmet item

# ── Header ────────────────────────────────────────────────────────────────────
echo "=== PR Documentation Audit ==="
echo "    Base:        $BASE  (merge-base: ${MERGE_BASE:0:12})"
echo "    Branch:      $(git rev-parse --abbrev-ref HEAD)"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Item 1 — Traceability matrix (REQUIRED when src/ or tests/ changed)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- Item 1: Traceability (docs/TRACEABILITY_MATRIX.md) ---"
ITEM1_MARK=""
if [ -z "$SRC_CHANGED" ] && [ -z "$TEST_CHANGED" ]; then
    echo "    Not triggered (no src/ or tests/ changes)."
    ITEM1_MARK="- N/A — no src/ or tests/ changes in this PR"
else
    echo "    Triggered: src/ or tests/ files changed."
    echo "    Running bash docs/check_traceability.sh ..."
    TRACE_LOG="$(bash docs/check_traceability.sh 2>&1)" && TRACE_PASS=0 || TRACE_PASS=$?
    if [ "$TRACE_PASS" -eq 0 ]; then
        echo "    check_traceability: PASS"
        if doc_updated "docs/TRACEABILITY_MATRIX.md"; then
            echo "    TRACEABILITY_MATRIX.md: committed in this branch — OK"
            ITEM1_MARK="- [x] \`docs/TRACEABILITY_MATRIX.md\` regenerated (\`bash docs/check_traceability.sh\`)"
        else
            echo "    TRACEABILITY_MATRIX.md: not committed — regenerate and commit it"
            echo "      (run: bash docs/check_traceability.sh, then update and git add docs/TRACEABILITY_MATRIX.md)"
            ITEM1_MARK="- [ ] \`docs/TRACEABILITY_MATRIX.md\` regenerated (\`bash docs/check_traceability.sh\`)"
            OVERALL_FAIL=1
        fi
    else
        echo "    check_traceability: FAIL"
        printf '%s\n' "$TRACE_LOG" | tail -20 | sed 's/^/      /'
        ITEM1_MARK="- [ ] \`docs/TRACEABILITY_MATRIX.md\` regenerated (\`bash docs/check_traceability.sh\`)"
        OVERALL_FAIL=1
    fi
fi
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Item 2 — Coverage ceilings (WARN — pre-merge human gate, not hard CI block)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- Item 2: Coverage ceilings (docs/COVERAGE_CEILINGS.md) ---"
ITEM2_MARK=""
if [ -z "$NONTRIVIAL_SRC" ]; then
    echo "    Not triggered (no src/ changes)."
    ITEM2_MARK="- N/A — no src/ changes in this PR"
else
    echo "    Triggered: src/ files changed (pre-merge coverage run required)."
    if doc_updated "docs/COVERAGE_CEILINGS.md"; then
        echo "    COVERAGE_CEILINGS.md: updated in this branch — OK"
        ITEM2_MARK="- [x] \`docs/COVERAGE_CEILINGS.md\` updated after \`make coverage\`"
    else
        echo "    COVERAGE_CEILINGS.md: not updated — run 'make coverage' and append a round section"
        # Coverage update is a human-reviewer gate (CLAUDE.md §1c), not a hard audit block.
        ITEM2_MARK="- [ ] \`docs/COVERAGE_CEILINGS.md\` updated after \`make coverage\`"
    fi
fi
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Item 3 — Hazard analysis (REQUIRED when SC-annotated files changed)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- Item 3: Hazard analysis (docs/HAZARD_ANALYSIS.md) ---"
ITEM3_MARK=""
if [ -z "$SC_FILES_CHANGED" ]; then
    echo "    Not triggered (no SC-annotated files changed)."
    ITEM3_MARK="- N/A — no SC-annotated files changed in this PR"
else
    echo "    Triggered: SC-annotated files changed:"
    printf '%s\n' "$SC_FILES_CHANGED" | sed 's/^/      /'
    if doc_updated "docs/HAZARD_ANALYSIS.md"; then
        echo "    HAZARD_ANALYSIS.md: updated in this branch — OK"
        ITEM3_MARK="- [x] \`docs/HAZARD_ANALYSIS.md\` reviewed; SC annotations added where needed"
    else
        echo "    HAZARD_ANALYSIS.md: NOT updated — review HAZ entries and FMEA rows"
        ITEM3_MARK="- [ ] \`docs/HAZARD_ANALYSIS.md\` reviewed; SC annotations added where needed"
        OVERALL_FAIL=1
    fi
fi
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Item 4 — State machines (REQUIRED when SM component files changed)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- Item 4: State machines (docs/STATE_MACHINES.md) ---"
ITEM4_MARK=""
if [ -z "$SM_FILES_CHANGED" ]; then
    echo "    Not triggered (no state machine component files changed)."
    ITEM4_MARK="- N/A — no state machine component files changed in this PR"
else
    echo "    Triggered: state machine component files changed:"
    printf '%s\n' "$SM_FILES_CHANGED" | sed 's/^/      /'
    if doc_updated "docs/STATE_MACHINES.md"; then
        echo "    STATE_MACHINES.md: updated in this branch — OK"
        ITEM4_MARK="- [x] \`docs/STATE_MACHINES.md\` updated"
    else
        echo "    STATE_MACHINES.md: NOT updated — update transition tables and invariants"
        ITEM4_MARK="- [ ] \`docs/STATE_MACHINES.md\` updated"
        OVERALL_FAIL=1
    fi
fi
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Item 5 — INSP record (REQUIRED for any non-trivial src/ change)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- Item 5: INSP record (docs/DEFECT_LOG.md) ---"
ITEM5_MARK=""
if [ -z "$NONTRIVIAL_SRC" ]; then
    echo "    Not triggered (no non-trivial src/ changes)."
    ITEM5_MARK="- N/A — no src/ changes in this PR"
else
    echo "    Triggered: non-trivial src/ changes detected."
    if doc_updated "docs/DEFECT_LOG.md"; then
        echo "    DEFECT_LOG.md: updated in this branch — OK"
        ITEM5_MARK="- [x] \`docs/DEFECT_LOG.md\` INSP record filed"
    else
        echo "    DEFECT_LOG.md: NOT updated — file an INSP record (docs/INSPECTION_CHECKLIST.md)"
        ITEM5_MARK="- [ ] \`docs/DEFECT_LOG.md\` INSP record filed"
        OVERALL_FAIL=1
    fi
fi
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# Item 6 — Stress tests (WARN — must be run by human; can't run here)
# ─────────────────────────────────────────────────────────────────────────────
echo "--- Item 6: Stress tests (make run_stress_tests) ---"
ITEM6_MARK=""
if [ -z "$TYPES_CHANGED" ] && [ -z "$LOOP_FILES_CHANGED" ]; then
    echo "    Not triggered (no capacity constant or loop algorithm changes)."
    ITEM6_MARK="- N/A — no capacity constant or loop algorithm changes in this PR"
else
    if [ -n "$TYPES_CHANGED" ]; then
        echo "    Triggered: src/core/Types.hpp (capacity constants) changed."
    fi
    if [ -n "$LOOP_FILES_CHANGED" ]; then
        echo "    Triggered: loop algorithm files changed:"
        printf '%s\n' "$LOOP_FILES_CHANGED" | sed 's/^/      /'
    fi
    echo "    ACTION REQUIRED: run 'make run_stress_tests' and confirm all suites pass."
    # Stress tests are not run here (too slow for pre-PR scripting); human must confirm.
    ITEM6_MARK="- [ ] \`make run_stress_tests\` passed (REQUIRED — capacity constant or loop algorithm changed)"
fi
echo ""

# ── Summary ───────────────────────────────────────────────────────────────────
echo "=== Summary ==="
RESULT="PASS"
[ "$OVERALL_FAIL" -ne 0 ] && RESULT="FAIL"
echo "    Overall result: $RESULT"
echo ""

# ── Filled-in PR template block ───────────────────────────────────────────────
echo "─────────────────────────────────────────────────────────────────────────"
echo "Paste the block below into the PR description:"
echo "─────────────────────────────────────────────────────────────────────────"
echo ""
echo "## Documentation updated ([docs/DOC_MAINTENANCE.md §2](docs/DOC_MAINTENANCE.md))"
echo ""
echo "$ITEM1_MARK"
echo "$ITEM2_MARK"
echo "$ITEM3_MARK"
echo "$ITEM4_MARK"
echo "$ITEM5_MARK"
echo "$ITEM6_MARK"
echo ""

if [ "$OVERALL_FAIL" -ne 0 ]; then
    echo "=== RESULT: FAIL — complete the items marked '- [ ]' above before opening the PR ==="
    exit 1
fi
echo "=== RESULT: PASS ==="
exit 0
