// Copyright 2026 Don Jessup
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file Version.hpp
 * @brief Compile-time project version constants for messageEngine.
 *
 * These constants are the single source of truth for the library version.
 * They must be kept in sync with the git tag on every release — the
 * `make check_version` target enforces this mechanically.
 *
 * Versioning policy (SemVer):
 *   MAJOR — breaking API change (TransportInterface, MessageEnvelope fields,
 *            any SC function signature change).
 *   MINOR — new capability added backward-compatibly (new transport backend,
 *            new SC function, new requirement coverage).
 *   PATCH — bug fix, documentation correction, coverage improvement; no API
 *            or wire format change.
 *
 * NOTE: This is the library (software artifact) version.
 *       The on-wire protocol version is separate: src/core/ProtocolVersion.hpp.
 *       A PATCH library bump never requires a PROTO_VERSION bump.
 *       A wire format change always requires both a MAJOR/MINOR library bump
 *       AND a PROTO_VERSION bump.
 *
 * Version history:
 *   1.0.0 — initial release: Apache 2.0 license, protocol versioning
 *            (PROTO_VERSION=1), capacity stress tests.
 *
 * NSC-infrastructure: compile-time version constants only; no requirement
 *   implementation belongs here. No REQ-x.x tag applies.
 *
 * Rules applied:
 *   - Power of 10 rule 8: compile-time constants only; no macros that affect
 *     control flow.
 *   - F-Prime style: no templates, no STL, no dynamic allocation.
 */

#ifndef CORE_VERSION_HPP
#define CORE_VERSION_HPP

#include <cstdint>

static const uint32_t ME_VERSION_MAJOR  = 1U;
static const uint32_t ME_VERSION_MINOR  = 0U;
static const uint32_t ME_VERSION_PATCH  = 0U;

/// Packed single integer for compile-time range checks: (major<<16)|(minor<<8)|patch
static const uint32_t ME_VERSION_NUMBER =
    (ME_VERSION_MAJOR << 16U) | (ME_VERSION_MINOR << 8U) | ME_VERSION_PATCH;

/// Human-readable version string — must match the git tag vMAJOR.MINOR.PATCH exactly.
/// The `make check_version` target verifies this at release time.
static const char ME_VERSION_STRING[] = "1.0.0";

#endif // CORE_VERSION_HPP
