## Summary

<!-- Describe what this PR does and why. Reference issues with "Fixes #NNN". -->

## Change type

<!-- Check one. This determines which documentation rows in docs/DOC_MAINTENANCE.md apply. -->

- [ ] Capacity constant changed (`src/core/Types.hpp`)
- [ ] New or changed public function
- [ ] SC state machine change
- [ ] Wire format change
- [ ] TLS/DTLS backend change
- [ ] New transport backend or platform adapter
- [ ] Logger / logging infrastructure change
- [ ] Build system change (Makefile, CMakeLists.txt)
- [ ] Standards or coding policy change
- [ ] Documentation-only change
- [ ] Other (describe below)

## Pre-merge checklist

<!-- Required items must be checked before this PR can be reviewed.            -->
<!-- Conditional items must be checked OR marked N/A with a brief reason.      -->
<!-- See docs/DOC_MAINTENANCE.md §2 for the full per-change-type update table. -->

### CI gates (always required)

- [ ] `make lint` — PASS
- [ ] `make run_tests` — all tests green
- [ ] `make check_traceability` — RESULT: PASS
- [ ] All `Implements:` / `Verifies:` tags present in changed files
- [ ] No raw `assert()` in `src/` — `NEVER_COMPILED_OUT_ASSERT` used throughout
- [ ] `docs/TRACEABILITY_MATRIX.md` regenerated (`bash docs/check_traceability.sh`)

### Conditional gates

- [ ] `make coverage` — PASS; `docs/COVERAGE_CEILINGS.md` updated if branch counts changed — or **N/A:**
- [ ] `make run_stress_tests` — PASS (required if capacity constant or loop algorithm changed) — or **N/A:**
- [ ] `docs/HAZARD_ANALYSIS.md` reviewed; SC annotations added where needed — or **N/A:**
- [ ] `docs/STATE_MACHINES.md` updated — or **N/A:**
- [ ] `docs/MCDC_ANALYSIS.md` updated — or **N/A:**
- [ ] `docs/DEFECT_LOG.md` INSP record filed — or **N/A:**
