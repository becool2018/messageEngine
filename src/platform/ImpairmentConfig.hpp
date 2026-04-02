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
 * @file ImpairmentConfig.hpp (platform/ forwarding shim)
 * @brief ImpairmentConfig has been moved to src/core/ImpairmentConfig.hpp.
 *
 * This file is retained only so that legacy includes from docs/ examples and
 * external tooling that reference "platform/ImpairmentConfig.hpp" continue to
 * compile. All production includes should use "core/ImpairmentConfig.hpp".
 *
 * Layering rationale: ImpairmentConfig is now embedded in ChannelConfig (a
 * core type) and therefore must reside in src/core/ to avoid a core → platform
 * dependency violation per CLAUDE.md §3 Architecture rules.
 *
 * Implements: REQ-5.2.1
 */

#ifndef PLATFORM_IMPAIRMENT_CONFIG_HPP
#define PLATFORM_IMPAIRMENT_CONFIG_HPP

// Re-export everything from the canonical location.
#include "core/ImpairmentConfig.hpp"

#endif // PLATFORM_IMPAIRMENT_CONFIG_HPP
