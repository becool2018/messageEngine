# Snyk.io Security Check

## Ignored Issues

| Field       | Value |
|-------------|-------|
| Severity    | LOW — Authentication Bypass by Spoofing |
| Status      | IGNORED |
| Finding ID  | ac7fb8aa-61a3-4281-9b45-15dd93d881af |
| Path        | tests/test_DtlsUdpBackend.cpp, line 1524 |
| Info        | A hardcoded domain name is compared in strncmp. This check could lead to a bypass since the domain name can be spoofed or controlled by an attacker. |
| Expiration  | never |
| Category    | not-vulnerable |
| Ignored on  | April 09, 2026 |
| Ignored by  | djessup72@yahoo.com |
| Reason      | Test code — false positive |

## Test Summary

| Field          | Value |
|----------------|-------|
| Organization   | becool2018 |
| Test type      | Static code analysis |
| Project path   | /Users/donjessup/2ndMessageEngine/messageEngine |
| Total issues   | 1 |
| Ignored issues | 1 — 0 HIGH / 0 MEDIUM / 1 LOW |
| Open issues    | 0 — 0 HIGH / 0 MEDIUM / 0 LOW |

## Report

Results: https://app.snyk.io/org/becool2018/project/e205bd5e-09c6-4fdf-8c62-7f4875603022/history/d162212b-c235-47cd-b2f9-5b0a881d836f

## Scan Metadata

| Field         | Value |
|---------------|-------|
| Trunk commit  | 88a863d01ffbc0c89fd75584acbaf5a0eb89533f |
| Commit date   | 2026-04-09 14:13:19 -0600 |
| Scanned on    | 2026-04-09 |
