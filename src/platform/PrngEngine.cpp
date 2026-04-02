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
 * @file PrngEngine.cpp
 * @brief Implementation of xorshift64-based PRNG.
 *
 * Note: This file is primarily for documentation. The PRNG implementation
 * is header-only (inline) for maximum inlining by the compiler.
 *
 * Rules applied:
 *   - All PRNG logic is in PrngEngine.hpp as inline methods.
 *   - This file exists to satisfy build conventions.
 *
 * Implements: REQ-5.2.4, REQ-5.3.1
 */

#include "PrngEngine.hpp"
#include "core/Assert.hpp"

// Empty implementation file; all logic is in PrngEngine.hpp (inline).
// This file can remain empty or contain only the comment above.
