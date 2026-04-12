#!/bin/bash
# check_traceability.sh
# Validates bidirectional traceability between CLAUDE.md REQ IDs,
# src/ Implements comments, and tests/ Verifies comments.
# Usage: bash docs/check_traceability.sh
# Policy: CLAUDE.md §11

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLAUDE_MD="$REPO_ROOT/CLAUDE.md"
SRC_DIR="$REPO_ROOT/src"
TESTS_DIR="$REPO_ROOT/tests"

# ── Known unimplemented requirements ─────────────────────────────────────────
# Add REQ IDs here only for requirements that are intentionally not yet
# implemented (document why and which PR will implement them).
#
# Security fix PRs 1-5 (branch fix/security-step0-2026-04 forward):
#   REQ-3.2.10 — C-3/C-4: frame_len / total_payload_length overflow guard
#                Implemented in: PR 1 (SocketUtils.cpp, ReassemblyBuffer.cpp)
#   REQ-3.2.11 — C-1/C-2: constant-time equality comparator
#                Implemented in: PR 2 (AckTracker, DuplicateFilter, OrderingBuffer,
#                                      DtlsUdpBackend) — DONE
#   REQ-5.2.6  — H-6: entropy source failure → FATAL in production
#                Implemented in: PR 5 (DeliveryEngine.cpp)
#   REQ-6.1.12 — H-5: HELLO timeout slot eviction
#                Implemented in: PR 5 (TcpBackend.cpp, TlsTcpBackend.cpp)
#   REQ-6.2.5  — H-7: UdpBackend wildcard peer_ip rejection
#                Implemented in: PR 5 (UdpBackend.cpp)
#   REQ-6.3.6  — H-1: empty CA fatal when verify_peer=true
#                Implemented in: PR 3 (TlsTcpBackend.cpp, DtlsUdpBackend.cpp)
#   REQ-6.3.7  — H-2: require_crl enforcement (new TlsConfig field)
#                Implemented in: PR 3 (TlsTcpBackend.cpp, DtlsUdpBackend.cpp)
#   REQ-6.3.8  — H-4: forward secrecy TLS 1.2 resumption rejection
#                Implemented in: PR 3 (TlsTcpBackend.cpp)
#   REQ-6.3.9  — H-8: verify_peer=false + non-empty hostname rejection
#                Implemented in: PR 3 (TlsTcpBackend.cpp, DtlsUdpBackend.cpp)
#   REQ-6.3.10 — H-3: TlsSessionStore POSIX mutex for concurrent access
#                Implemented in: PR 4 (TlsSessionStore.cpp)
KNOWN_GAPS="REQ-5.2.6 REQ-6.1.12 REQ-6.2.5"

PASS=0
FAIL=0

# ── 1. Extract all REQ IDs defined in CLAUDE.md ─────────────────────────────
defined_ids=$(grep -oE '\[REQ-[0-9]+\.[0-9.]+\]' "$CLAUDE_MD" | tr -d '[]' | sort -u)
echo "=== Defined REQ IDs ($(echo "$defined_ids" | wc -l | tr -d ' ')) ==="
echo "$defined_ids"
echo ""

# ── 2. Extract all Implements: tags from src/ ────────────────────────────────
echo "=== Implements tags in src/ ==="
implemented_ids=$(grep -rh "Implements:" "$SRC_DIR" | grep -oE 'REQ-[0-9]+\.[0-9.]+' | sort -u || true)
echo "$implemented_ids"
echo ""

# ── 3. Extract all Verifies: tags from tests/ ───────────────────────────────
echo "=== Verifies tags in tests/ ==="
verified_ids=$(grep -rh "Verifies:" "$TESTS_DIR" | grep -oE 'REQ-[0-9]+\.[0-9.]+' | sort -u || true)
echo "$verified_ids"
echo ""

# ── 4. Check: every defined REQ ID has at least one Implements tag ───────────
echo "=== REQ IDs with no Implements tag (coverage gap) ==="
gap_count=0
while IFS= read -r req; do
    if ! echo "$implemented_ids" | grep -qx "$req"; then
        if echo "$KNOWN_GAPS" | grep -qw "$req"; then
            echo "  KNOWN GAP (not yet implemented): $req"
            gap_count=$((gap_count + 1))
        else
            echo "  MISSING Implements: $req"
            FAIL=$((FAIL + 1))
        fi
    fi
done <<< "$defined_ids"
[ $FAIL -eq 0 ] && echo "  (none unexpected)" && PASS=$((PASS + 1))
echo ""

# ── 5. Check: every src/ Implements tag references a defined REQ ID ──────────
echo "=== Implements tags referencing undefined REQ IDs (orphan) ==="
orphan=0
while IFS= read -r req; do
    if ! echo "$defined_ids" | grep -qx "$req"; then
        echo "  ORPHAN Implements: $req (not in CLAUDE.md)"
        orphan=$((orphan + 1))
        FAIL=$((FAIL + 1))
    fi
done <<< "$implemented_ids"
[ $orphan -eq 0 ] && echo "  (none)" && PASS=$((PASS + 1))
echo ""

# ── 6. Check: every tests/ Verifies tag references a defined REQ ID ──────────
echo "=== Verifies tags referencing undefined REQ IDs (orphan) ==="
orphan=0
while IFS= read -r req; do
    if ! echo "$defined_ids" | grep -qx "$req"; then
        echo "  ORPHAN Verifies: $req (not in CLAUDE.md)"
        orphan=$((orphan + 1))
        FAIL=$((FAIL + 1))
    fi
done <<< "$verified_ids"
[ $orphan -eq 0 ] && echo "  (none)" && PASS=$((PASS + 1))
echo ""

# ── 7. List src/ files missing any Implements tag ───────────────────────────
echo "=== src/ files missing Implements tag ==="
missing=0
while IFS= read -r f; do
    # Skip files marked as NSC-infrastructure (no REQ-x.x applies)
    if grep -q "NSC-infrastructure:" "$f"; then
        continue
    fi
    if ! grep -q "Implements:" "$f"; then
        echo "  MISSING: ${f#$REPO_ROOT/}"
        missing=$((missing + 1))
        FAIL=$((FAIL + 1))
    fi
done < <(find "$SRC_DIR" -name "*.cpp" -o -name "*.hpp" | sort)
[ $missing -eq 0 ] && echo "  (none)" && PASS=$((PASS + 1))
echo ""

# ── Summary ──────────────────────────────────────────────────────────────────
echo "=== Summary ==="
echo "  Checks passed: $PASS"
echo "  Checks failed: $FAIL"
[ $FAIL -eq 0 ] && echo "  RESULT: PASS" && exit 0
echo "  RESULT: FAIL" && exit 1
