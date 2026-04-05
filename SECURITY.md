# Security Policy

## Supported versions

| Version | Supported |
|---------|-----------|
| 2.x     | Yes       |
| < 2.0   | No        |

## Reporting a vulnerability

**Do not open a public GitHub issue for security vulnerabilities.**

messageEngine is a safety-critical networking library. A publicly disclosed
vulnerability before a fix is available puts all users at risk.

### How to report

Use **GitHub Private Security Advisories**:

1. Go to the [Security tab](../../security/advisories) of this repository.
2. Click **"Report a vulnerability"**.
3. Fill in the advisory form with as much detail as possible (see below).
4. Submit — only repository maintainers can see it.

### What to include

- **Description** — what the vulnerability is and which component is affected.
- **Impact** — what an attacker or faulty sender could do (e.g., buffer overread,
  denial of service, incorrect message delivery).
- **Reproduction steps** — minimal code or packet sequence that triggers the issue.
- **Affected versions** — which release(s) you tested against.
- **Suggested fix** (optional) — if you have one.

### Response timeline

| Event | Target |
|-------|--------|
| Acknowledgement | Within 5 business days |
| Triage and severity assignment | Within 10 business days |
| Fix and patched release | Depends on severity; critical issues prioritised |
| Public advisory published | After patched release is available |

### Severity guidance

This project follows the CVSS v3.1 severity scale. Issues affecting safety-critical
send/receive paths (`DeliveryEngine::send`, `DeliveryEngine::receive`,
`Serializer::deserialize`) are treated as **Critical** regardless of CVSS score,
because incorrect message delivery in an embedded context can have safety consequences
beyond the software itself.

### Out of scope

- Issues in third-party dependencies (mbedTLS, system libc). Report those upstream.
- Theoretical vulnerabilities with no practical exploit path.
- Issues only reproducible with a deliberately malicious build configuration.
