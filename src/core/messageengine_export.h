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
 * @file messageengine_export.h
 * @brief Symbol visibility macro for shared-library builds.
 *
 * ME_API controls symbol export/import behaviour across platforms:
 *
 *   Static library consumers  (-DMESSAGEENGINE_STATIC_DEFINE):
 *     ME_API expands to nothing — zero effect on compilation.
 *
 *   Shared library build      (-DMESSAGEENGINE_BUILD_SHARED, set by Makefile shared_lib target):
 *     Linux/macOS: ME_API = __attribute__((visibility("default")))
 *                  Marks the symbol as exported from the .so / .dylib.
 *     Windows:     ME_API = __declspec(dllexport)
 *
 *   Shared library consumer   (neither define set):
 *     Linux/macOS: ME_API expands to nothing (symbols found via normal dynamic linking).
 *     Windows:     ME_API = __declspec(dllimport)
 *
 * Usage:
 *   class ME_API MyClass { ... };
 *   ME_API void my_function();
 *
 * Note: the shared library Makefile target does NOT use -fvisibility=hidden in this
 * initial release; all symbols are exported by default. ME_API annotations are present
 * as infrastructure for a future hardened ABI pass and for Windows portability.
 *
 * NSC-infrastructure: packaging visibility control; no requirement implementation.
 * No REQ-x.x tag applies.
 *
 * Rules applied:
 *   - Power of 10 rule 8: preprocessor macro used only for portability decoration,
 *     not for control flow.
 */

#ifndef MESSAGEENGINE_EXPORT_H
#define MESSAGEENGINE_EXPORT_H

#ifdef MESSAGEENGINE_STATIC_DEFINE
    /* Static library: macro is a no-op. */
#   define ME_API

#elif defined(_WIN32)
    /* Windows shared library. */
#   ifdef MESSAGEENGINE_BUILD_SHARED
#       define ME_API __declspec(dllexport)
#   else
#       define ME_API __declspec(dllimport)
#   endif

#else
    /* Linux / macOS shared library or default (symbols visible by default). */
#   ifdef MESSAGEENGINE_BUILD_SHARED
#       define ME_API __attribute__((visibility("default")))
#   else
#       define ME_API
#   endif

#endif /* MESSAGEENGINE_STATIC_DEFINE */

#endif /* MESSAGEENGINE_EXPORT_H */
