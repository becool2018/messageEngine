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
 */

#ifndef PLATFORM_IMPAIRMENT_CONFIG_HPP
#define PLATFORM_IMPAIRMENT_CONFIG_HPP

// Re-export everything from the canonical location.
#include "core/ImpairmentConfig.hpp"

#endif // PLATFORM_IMPAIRMENT_CONFIG_HPP
