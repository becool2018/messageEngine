# Detect OS
UNAME_S := $(shell uname -s)

# ─────────────────────────────────────────────────────────────────────────────
# Installation paths — Yocto do_install sets DESTDIR=${D}; prefix defaults to
# /usr which matches the Yocto standard layout.  Override on the command line:
#   make install DESTDIR=/path/to/staging prefix=/usr
# ─────────────────────────────────────────────────────────────────────────────
DESTDIR ?=
prefix  ?= /usr
bindir  ?= $(prefix)/bin

# ─────────────────────────────────────────────────────────────────────────────
# Release profile — set RELEASE=1 for production / Yocto target builds.
# Activates -O2 (required for _FORTIFY_SOURCE=2) and -D_FORTIFY_SOURCE=2.
# Debug builds (default) omit both to preserve accurate line-level debug info.
# CLAUDE.md §7e: -D_FORTIFY_SOURCE=2 requires -O1+; PENDING in debug builds.
# ─────────────────────────────────────────────────────────────────────────────
RELEASE ?= 0
ifeq ($(RELEASE),1)
RELEASE_CXXFLAGS := -O2 -D_FORTIFY_SOURCE=2
else
RELEASE_CXXFLAGS := -g -O0
endif

# ─────────────────────────────────────────────────────────────────────────────
# Extra flag hooks for cross-compilation environments (e.g. Yocto).
# Yocto recipes pass sysroot paths, target tune flags, and security hardening
# via these variables:
#   EXTRA_CXXFLAGS  — appended to CXXFLAGS after all project-required flags
#   EXTRA_LDFLAGS   — appended to LDFLAGS / EXE_LDFLAGS after library flags
# Example Yocto do_compile override:
#   oe_runmake CXX="${CXX}" \
#       MBEDTLS_CFLAGS="-I${STAGING_INCDIR}" \
#       MBEDTLS_LIBS="-L${STAGING_LIBDIR} -lmbedtls -lmbedx509 -lmbedcrypto" \
#       EXTRA_CXXFLAGS="${TARGET_CXXFLAGS}" \
#       EXTRA_LDFLAGS="${TARGET_LDFLAGS}" \
#       RELEASE=1
# ─────────────────────────────────────────────────────────────────────────────
EXTRA_CXXFLAGS ?=
EXTRA_LDFLAGS  ?=

# ─────────────────────────────────────────────────────────────────────────────
# TLS/DTLS support — set TLS=0 to build without mbedTLS.
# When TLS=0:
#   • TlsTcpBackend, TlsSessionStore, DtlsUdpBackend, MbedtlsOpsImpl are
#     excluded from compilation and linking.
#   • mbedTLS include/link flags are omitted entirely.
#   • -DMESSAGEENGINE_NO_TLS is injected so source/headers can guard TLS APIs.
#   • test_TlsTcpBackend and test_DtlsUdpBackend are excluded from all targets.
#   • tls_demo, dtls_demo, and the demos umbrella target are disabled.
# Default: TLS=1 (mbedTLS required).
# ─────────────────────────────────────────────────────────────────────────────
TLS ?= 1
ifeq ($(TLS),1)
$(info [messageEngine] TLS/DTLS: ENABLED  — TlsTcpBackend + DtlsUdpBackend compiled in, mbedTLS linked)
else
$(info [messageEngine] TLS/DTLS: DISABLED — TlsTcpBackend + DtlsUdpBackend excluded, no mbedTLS required)
endif

# Optional tool for discovering system-provided mbedTLS flags.
PKG_CONFIG ?= pkg-config

# Prefer Homebrew LLVM on macOS if it exists, otherwise use PATH
LLVM_BIN_BREW := /opt/homebrew/opt/llvm/bin
LLVM_BIN      := $(if $(wildcard $(LLVM_BIN_BREW)/clang++),$(LLVM_BIN_BREW),)

# ─────────────────────────────────────────────────────────────────────────────
# Makefile for messageEngine
# Enforces Power of 10 zero-warnings policy (rule 10).
# No cmake required; pure GNU make.
# ─────────────────────────────────────────────────────────────────────────────

# Tools (fall back to PATH if LLVM_BIN is empty)
CLANG_TIDY   := $(if $(LLVM_BIN),$(LLVM_BIN)/clang-tidy,clang-tidy)
SCAN_BUILD   := $(if $(LLVM_BIN),$(LLVM_BIN)/scan-build,scan-build)
# scan-build requires an absolute path for --use-analyzer on Linux.
# Use LLVM_BIN/clang on macOS (Homebrew LLVM) or resolve via 'which' on Linux.
SCAN_ANALYZER := $(if $(LLVM_BIN),$(LLVM_BIN)/clang,$(shell which clang 2>/dev/null))
COV_CXX      := $(if $(LLVM_BIN),$(LLVM_BIN)/clang++,clang++)
LLVM_PROFDATA:= $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-profdata,llvm-profdata)
LLVM_COV     := $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-cov,llvm-cov)

# Compiler
CXX       ?= g++

# Detect compiler family to handle ##__VA_ARGS__ zero-arg extension.
# Clang: -Wpedantic + -Wno-gnu-zero-variadic-macro-arguments suppresses just
#   the GNU-extension warning while keeping all other pedantic checks.
# GCC:   -Wpedantic has no subcategory flag for the zero-arg ##__VA_ARGS__
#   warning; the only option is to omit -Wpedantic on GCC builds.
#   -Wno-variadic-macros is kept on GCC to silence the non-pedantic form.
CXX_IS_CLANG := $(findstring clang,$(shell $(CXX) --version 2>/dev/null))
ifeq ($(CXX_IS_CLANG),)
  # GCC path: no -Wpedantic, suppress variadic-macros warning non-pedantically
  PEDANTIC_FLAG        :=
  VARIADIC_MACROS_FLAG := -Wno-variadic-macros
else
  # Clang path: keep -Wpedantic, suppress just the GNU ##__VA_ARGS__ extension
  PEDANTIC_FLAG        := -Wpedantic
  VARIADIC_MACROS_FLAG := -Wno-gnu-zero-variadic-macro-arguments
endif

ifeq ($(TLS),1)
# mbedTLS include/lib discovery:
# 1) Prefer pkg-config when available (Linux distros, some macOS setups)
# 2) On macOS, fall back to Homebrew's opt prefix when pkg-config data is absent
# 3) Last resort: plain linker names for environments with default system paths
MBEDTLS_PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags mbedtls mbedx509 mbedcrypto 2>/dev/null)
MBEDTLS_PKG_LIBS   := $(shell $(PKG_CONFIG) --libs mbedtls mbedx509 mbedcrypto 2>/dev/null)

MBEDTLS_FALLBACK_CFLAGS :=
MBEDTLS_FALLBACK_LIBS   :=

ifeq ($(UNAME_S),Darwin)
HOMEBREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(HOMEBREW_PREFIX),)
MBEDTLS_BREW_PREFIX := $(HOMEBREW_PREFIX)/opt/mbedtls
ifneq ($(wildcard $(MBEDTLS_BREW_PREFIX)/include/mbedtls/ssl.h),)
MBEDTLS_FALLBACK_CFLAGS := -I$(MBEDTLS_BREW_PREFIX)/include
MBEDTLS_FALLBACK_LIBS   := -L$(MBEDTLS_BREW_PREFIX)/lib -lmbedtls -lmbedx509 -lmbedcrypto
endif
endif
endif

MBEDTLS_CFLAGS ?= $(if $(strip $(MBEDTLS_PKG_CFLAGS)),$(MBEDTLS_PKG_CFLAGS),$(MBEDTLS_FALLBACK_CFLAGS))
else
# TLS=0: mbedTLS not used; clear all TLS-related flags so nothing leaks into
# CXXFLAGS or LDFLAGS and the build succeeds without mbedTLS installed.
MBEDTLS_CFLAGS :=
MBEDTLS_LIBS   :=
endif

# Include path: do NOT hardcode /opt/homebrew/include by default
CXXFLAGS  := -std=c++17 -fno-exceptions -fno-rtti \
             -Wall -Wextra $(PEDANTIC_FLAG) -Werror \
             -Wshadow -Wconversion -Wsign-conversion \
             -Wcast-align -Wformat=2 -Wnull-dereference \
             -Wdouble-promotion -Wno-unknown-pragmas \
             $(VARIADIC_MACROS_FLAG) \
             -fstack-protector-strong -fPIE \
             $(RELEASE_CXXFLAGS) \
             -Isrc $(MBEDTLS_CFLAGS) -g \
             $(EXTRA_CXXFLAGS)
# Security hardening (.claude/CLAUDE.md §7e):
#   -fstack-protector-strong  ACTIVE: stack canaries on functions with buffers >= 8 bytes.
#   -fPIE                     ACTIVE: position-independent code for all objects.
#   -D_FORTIFY_SOURCE=2       ACTIVE when RELEASE=1 (requires -O2, set via RELEASE_CXXFLAGS).
#   -pie                      Applied per-executable in EXE_LDFLAGS below.
#   -Wl,-z,relro -Wl,-z,now   Linux-only; applied per-executable in EXE_LDFLAGS below.
ifneq ($(TLS),1)
# TLS=0: tell all translation units that TLS/DTLS APIs are compiled out.
CXXFLAGS += -DMESSAGEENGINE_NO_TLS
endif

# mbedTLS linking (portable default). Users can override.
# On Ubuntu with libmbedtls-dev installed, these libs are on the default linker path.
# When TLS=0 MBEDTLS_LIBS was already set to empty above; ?= is a no-op.
MBEDTLS_LIBS ?= $(if $(strip $(MBEDTLS_PKG_LIBS)),$(MBEDTLS_PKG_LIBS),$(if $(strip $(MBEDTLS_FALLBACK_LIBS)),$(MBEDTLS_FALLBACK_LIBS),-lmbedtls -lmbedx509 -lmbedcrypto))
LDFLAGS      := -lpthread $(MBEDTLS_LIBS) $(EXTRA_LDFLAGS)

# Per-executable hardening flags (CLAUDE.md §7e).
# -Wl,-pie:              enables ASLR for the resulting executable.
#                        Passed via -Wl, to avoid -Wunused-command-line-argument
#                        when $(CXXFLAGS) (which includes -Werror) appears on the
#                        same link command line (Apple clang macOS behaviour).
# -Wl,-z,relro/now:      makes GOT/PLT read-only after startup (Linux only).
ifeq ($(UNAME_S),Linux)
EXE_LDFLAGS := -Wl,-pie -Wl,-z,relro -Wl,-z,now
else
EXE_LDFLAGS := -Wl,-pie
endif

# ─────────────────────────────────────────────────────────────────────────────
# Sanitizer flags (ASan + UBSan)
# -fno-sanitize-recover=all: any UB terminates the process immediately
#   (mirrors ASan default; ensures sanitized tests fail rather than warn-and-pass).
# On Linux, ASan enables LeakSanitizer by default.  mbedTLS allocates heap
#   internally and does not always free it before exit, producing false positives.
#   ASAN_OPTIONS=detect_leaks=0 suppresses LSan on Linux without disabling ASan or
#   UBSan.  Power of 10 Rule 3 prohibits heap allocation on critical paths, so
#   real leak bugs in our code are structurally impossible.
# On macOS, LSan is not shipped with Apple clang; no suppression needed.
# ─────────────────────────────────────────────────────────────────────────────
SAN_OBJ_DIR  := build/san-objs
SAN_CXXFLAGS := $(filter-out -Werror,$(CXXFLAGS)) \
                -fsanitize=address,undefined \
                -fno-sanitize-recover=all
SAN_LDFLAGS  := -fsanitize=address,undefined $(LDFLAGS)
SAN_LIB_OBJS  = $(patsubst src/%.cpp,$(SAN_OBJ_DIR)/%.o,$(ALL_LIB_SRC))
# Suppress mbedTLS LSan false positives on Linux; no-op on macOS (LSan absent).
ifeq ($(UNAME_S),Linux)
SAN_RUN_ENV  := ASAN_OPTIONS=detect_leaks=0
else
SAN_RUN_ENV  :=
endif

# ─────────────────────────────────────────────────────────────────────────────
# Source groups
# ─────────────────────────────────────────────────────────────────────────────
CORE_SRC := \
    src/core/Logger.cpp \
    src/core/Serializer.cpp \
    src/core/MessageId.cpp \
    src/core/Timestamp.cpp \
    src/core/DuplicateFilter.cpp \
    src/core/AckTracker.cpp \
    src/core/RetryManager.cpp \
    src/core/DeliveryEngine.cpp \
    src/core/RequestReplyEngine.cpp \
    src/core/AssertState.cpp \
    src/core/Fragmentation.cpp \
    src/core/ReassemblyBuffer.cpp \
    src/core/OrderingBuffer.cpp

PLATFORM_SRC := \
    src/platform/PosixLogClock.cpp \
    src/platform/PosixLogSink.cpp \
    src/platform/PrngEngine.cpp \
    src/platform/ImpairmentEngine.cpp \
    src/platform/ImpairmentConfigLoader.cpp \
    src/platform/SocketUtils.cpp \
    src/platform/TcpBackend.cpp \
    src/platform/UdpBackend.cpp \
    src/platform/LocalSimHarness.cpp \
    src/platform/SocketOpsImpl.cpp \
    src/platform/PosixSyscallsImpl.cpp

ifeq ($(TLS),1)
# TLS/DTLS backends and the shared mbedTLS ops implementation.
PLATFORM_SRC += \
    src/platform/TlsTcpBackend.cpp \
    src/platform/TlsSessionStore.cpp \
    src/platform/DtlsUdpBackend.cpp \
    src/platform/MbedtlsOpsImpl.cpp
endif

ALL_LIB_SRC := $(CORE_SRC) $(PLATFORM_SRC)

# ─────────────────────────────────────────────────────────────────────────────
# TLS/DTLS test and sanitizer bin lists — empty when TLS=0.
# Used by: tests, sanitize_tests, run_tests, run_sanitize.
# ─────────────────────────────────────────────────────────────────────────────
ifeq ($(TLS),1)
RUN_TESTS_FOOTER    := === ALL TESTS PASSED (24/24) ===
RUN_SANITIZE_FOOTER := === SANITIZER TESTS PASSED (24/24) ===
TLS_TEST_BINS  := build/test_TlsTcpBackend build/test_DtlsUdpBackend
TLS_SAN_BINS   := build/san/test_TlsTcpBackend build/san/test_DtlsUdpBackend
TLS_TEST_NAMES := TlsTcpBackend DtlsUdpBackend
# lint: no exclusions needed when TLS=1 (mbedTLS headers present).
TLS_LINT_EXCL_SRC  :=
TLS_LINT_EXCL_TEST :=
# cppcheck: no -i exclusions needed when TLS=1.
CPPCHECK_TLS_EXCL  :=
else
RUN_TESTS_FOOTER    := === ALL TESTS PASSED (22/24 — TLS=0: test_TlsTcpBackend + test_DtlsUdpBackend skipped) ===
RUN_SANITIZE_FOOTER := === SANITIZER TESTS PASSED (22/24 — TLS=0: test_TlsTcpBackend + test_DtlsUdpBackend skipped) ===
TLS_TEST_BINS  :=
TLS_SAN_BINS   :=
TLS_TEST_NAMES :=
# lint: exclude TLS source and demo files so clang-tidy does not require
# mbedTLS headers on systems where mbedTLS is not installed.
TLS_LINT_EXCL_SRC  := src/platform/TlsTcpBackend.cpp \
                       src/platform/TlsSessionStore.cpp \
                       src/platform/DtlsUdpBackend.cpp \
                       src/platform/MbedtlsOpsImpl.cpp \
                       src/app/TlsTcpDemo.cpp \
                       src/app/DtlsUdpDemo.cpp
TLS_LINT_EXCL_TEST := tests/test_TlsTcpBackend.cpp \
                       tests/test_DtlsUdpBackend.cpp
# cppcheck: -i flags exclude TLS files from the directory scan.
CPPCHECK_TLS_EXCL  := -i src/platform/TlsTcpBackend.cpp \
                       -i src/platform/TlsSessionStore.cpp \
                       -i src/platform/DtlsUdpBackend.cpp \
                       -i src/platform/MbedtlsOpsImpl.cpp \
                       -i src/app/TlsTcpDemo.cpp \
                       -i src/app/DtlsUdpDemo.cpp
endif

# ─────────────────────────────────────────────────────────────────────────────
# Object files
# ─────────────────────────────────────────────────────────────────────────────
BUILD_DIR := build/objs
CORE_OBJS    := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(CORE_SRC))
PLATFORM_OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(PLATFORM_SRC))
ALL_LIB_OBJS := $(CORE_OBJS) $(PLATFORM_OBJS)

# ─────────────────────────────────────────────────────────────────────────────
# Targets
# ─────────────────────────────────────────────────────────────────────────────
# ─────────────────────────────────────────────────────────────────────────────
# Library version — extracted from src/core/Version.hpp at eval time.
# Used by static_lib, shared_lib, and package targets.
# ─────────────────────────────────────────────────────────────────────────────
ME_VERSION := $(shell grep 'ME_VERSION_STRING\[\]' src/core/Version.hpp \
                      | sed 's/.*"\(.*\)".*/\1/')
ME_MAJOR   := $(shell grep 'ME_VERSION_MAJOR  *=' src/core/Version.hpp \
                      | sed 's/.*=[ ]*\([0-9]*\)U.*/\1/')

# ─────────────────────────────────────────────────────────────────────────────
# Shared library naming — platform-specific.
#   Linux:  libmessageengine.so.MAJOR.MINOR.PATCH  (soname = .so.MAJOR)
#   macOS:  libmessageengine.MAJOR.MINOR.PATCH.dylib  (compatibility name = .MAJOR.dylib)
# ─────────────────────────────────────────────────────────────────────────────
ifeq ($(UNAME_S),Darwin)
SHARED_LIB      := build/libmessageengine.$(ME_VERSION).dylib
SHARED_SONAME   := libmessageengine.$(ME_MAJOR).dylib
SHARED_LDFLAGS  := -dynamiclib \
                   -install_name @rpath/$(SHARED_SONAME) \
                   -compatibility_version $(ME_MAJOR).0.0 \
                   -current_version $(ME_VERSION)
SHARED_SYMLINK1 := build/libmessageengine.$(ME_MAJOR).dylib
SHARED_SYMLINK2 := build/libmessageengine.dylib
else
SHARED_LIB      := build/libmessageengine.so.$(ME_VERSION)
SHARED_SONAME   := libmessageengine.so.$(ME_MAJOR)
SHARED_LDFLAGS  := -shared -Wl,-soname,$(SHARED_SONAME)
SHARED_SYMLINK1 := build/libmessageengine.so.$(ME_MAJOR)
SHARED_SYMLINK2 := build/libmessageengine.so
endif

# ─────────────────────────────────────────────────────────────────────────────
# PIC object tree — used exclusively for the shared library.
# Separate from build/objs/ (non-PIC) to avoid polluting executable objects.
# Power of 10 Rule 2 deviation: infrastructure build loop, bounded by ALL_LIB_SRC.
# ─────────────────────────────────────────────────────────────────────────────
PIC_OBJ_DIR      := build/pic-objs
PIC_CXXFLAGS     := $(filter-out -fPIE,$(CXXFLAGS)) -fPIC -DMESSAGEENGINE_BUILD_SHARED
PIC_CORE_OBJS    := $(patsubst src/%.cpp,$(PIC_OBJ_DIR)/%.o,$(CORE_SRC))
PIC_PLATFORM_OBJS := $(patsubst src/%.cpp,$(PIC_OBJ_DIR)/%.o,$(PLATFORM_SRC))
ALL_PIC_LIB_OBJS := $(PIC_CORE_OBJS) $(PIC_PLATFORM_OBJS)

# ─────────────────────────────────────────────────────────────────────────────
# Public header lists — used by install-dev and package targets.
# ─────────────────────────────────────────────────────────────────────────────
PUBLIC_HEADERS_CORE := \
    src/core/messageengine_export.h \
    src/core/MessageEnvelope.hpp \
    src/core/TransportInterface.hpp \
    src/core/DeliveryEngine.hpp \
    src/core/Serializer.hpp \
    src/core/Types.hpp \
    src/core/ChannelConfig.hpp \
    src/core/ImpairmentConfig.hpp \
    src/core/Logger.hpp \
    src/core/ProtocolVersion.hpp \
    src/core/Version.hpp \
    src/core/Assert.hpp \
    src/core/AssertState.hpp \
    src/core/IResetHandler.hpp \
    src/core/AbortResetHandler.hpp \
    src/core/DeliveryEvent.hpp \
    src/core/DeliveryEventRing.hpp \
    src/core/DeliveryStats.hpp \
    src/core/RingBuffer.hpp \
    src/core/MessageId.hpp \
    src/core/Timestamp.hpp \
    src/core/RequestReplyEngine.hpp \
    src/core/RequestReplyHeader.hpp

ifeq ($(TLS),1)
PUBLIC_HEADERS_CORE += src/core/TlsConfig.hpp
endif

PUBLIC_HEADERS_PLATFORM := \
    src/platform/TcpBackend.hpp \
    src/platform/LocalSimHarness.hpp \
    src/platform/ImpairmentEngine.hpp \
    src/platform/ImpairmentConfigLoader.hpp \
    src/platform/PrngEngine.hpp

ifeq ($(TLS),1)
PUBLIC_HEADERS_PLATFORM += \
    src/platform/TlsTcpBackend.hpp \
    src/platform/TlsSessionStore.hpp \
    src/platform/DtlsUdpBackend.hpp
endif

TESTING_HEADERS := \
    src/platform/ISocketOps.hpp \
    src/platform/IPosixSyscalls.hpp

ifeq ($(TLS),1)
TESTING_HEADERS += src/platform/IMbedtlsOps.hpp
endif

# ─────────────────────────────────────────────────────────────────────────────
# Installation paths for library + headers (extends existing bindir install).
# ─────────────────────────────────────────────────────────────────────────────
includedir ?= $(prefix)/include
libdir     ?= $(prefix)/lib

.PHONY: all clean install install-dev libs static_lib shared_lib package \
        tests stress_tests run_stress_tests run_tests server client \
        run_stress_reconnect run_stress_ringbuffer run_stress_tcp_fanin \
        tls_demo dtls_demo demos \
        check_traceability \
        lint cppcheck cppcheck-misra pclint scan_build static_analysis \
        coverage coverage_show coverage_report \
        sanitize_tests run_sanitize \
        check_version help

help:
	@echo ""
	@echo "messageEngine — available make targets"
	@echo ""
	@echo "Build:"
	@echo "  all                 Build server, client, and all tests (default)"
	@echo "  server              Build TCP echo server  (build/server)"
	@echo "  client              Build TCP echo client  (build/client)"
	@echo "  tls_demo            Build TLS/TCP demo     (build/tls_demo)"
	@echo "  dtls_demo           Build DTLS-UDP demo    (build/dtls_demo)"
	@echo "  demos               Build tls_demo and dtls_demo"
	@echo "  libs                Build static + shared libraries"
	@echo "  static_lib          Build static library (build/libmessageengine.a)"
	@echo "  shared_lib          Build shared library (build/libmessageengine.so / .dylib)"
	@echo "  install             Install server+client to prefix/bin  (override: DESTDIR, prefix, bindir)"
	@echo "  install-dev         Install headers + libs + pkg-config  (override: DESTDIR, prefix, includedir, libdir)"
	@echo "  package             Build distributable tarball  (build/messageengine-vVER-OS-ARCH.tar.gz)"
	@echo "  clean               Remove all build artifacts"
	@echo ""
	@echo "Test:"
	@echo "  tests               Build all 24 unit test binaries"
	@echo "  run_tests           Build and run the full unit test suite"
	@echo "  stress_tests         Build all stress tests (capacity, e2e, ordering)"
	@echo "  run_stress_tests     Build and run all stress tests (STRESS_DURATION=60)"
	@echo "  run_stress_capacity  Run capacity suite only       (STRESS_DURATION=60)"
	@echo "  run_stress_e2e       Run E2E suite only            (STRESS_DURATION=60)"
	@echo "  run_stress_ordering  Run ordering suite only       (STRESS_DURATION=60)"
	@echo "  run_stress_reconnect  Run reconnect suite only      (STRESS_DURATION=60)"
	@echo "  run_stress_ringbuffer  Run RingBuffer concurrent storm  (STRESS_DURATION=60)"
	@echo "  run_stress_tcp_fanin   Run TCP multi-client fan-in     (STRESS_DURATION=60)"
	@echo "  sanitize_tests      Build test suite with ASan + UBSan"
	@echo "  run_sanitize        Build and run ASan + UBSan test suite"
	@echo ""
	@echo "Static analysis:"
	@echo "  lint                Run clang-tidy on src/ and tests/"
	@echo "  cppcheck            Run cppcheck flow/style analysis"
	@echo "  cppcheck-misra      Run cppcheck + MISRA C++:2023 addon (macOS only)"
	@echo "  scan_build          Run Clang Static Analyzer (scan-build)"
	@echo "  static_analysis     Run lint + cppcheck + scan_build in sequence"
	@echo "  pclint              Run PC-lint Plus MISRA report (PENDING: licence required)"
	@echo ""
	@echo "Coverage:"
	@echo "  coverage            Build instrumented tests and show per-file coverage"
	@echo "  coverage_show       Annotated line-level output (run after coverage)"
	@echo "  coverage_report     Full per-file/per-function report with policy notes"
	@echo ""
	@echo "Verification:"
	@echo "  check_traceability  Verify REQ-ID Implements/Verifies tags"
	@echo "  check_version       Verify Version.hpp matches current git tag"
	@echo ""
	@echo "Options:"
	@echo "  RELEASE=1           Enable -O2 -D_FORTIFY_SOURCE=2 (production build)"
	@echo "  TLS=0               Build without TLS/DTLS (omits mbedTLS; injects -DMESSAGEENGINE_NO_TLS)"
	@echo "  DESTDIR=<path>      Install prefix  (Yocto: set to staging dir in do_install)"
	@echo "  CXX=<compiler>      Override compiler (default: g++)"
	@echo ""

all: server client tests

server: $(ALL_LIB_OBJS) build/objs/app/Server.o
	$(CXX) $(CXXFLAGS) -o build/server $^ $(LDFLAGS) $(EXE_LDFLAGS)

client: $(ALL_LIB_OBJS) build/objs/app/Client.o
	$(CXX) $(CXXFLAGS) -o build/client $^ $(LDFLAGS) $(EXE_LDFLAGS)

ifeq ($(TLS),1)
tls_demo: $(ALL_LIB_OBJS) build/objs/app/TlsTcpDemo.o
	$(CXX) $(CXXFLAGS) -o build/tls_demo $^ $(LDFLAGS)

dtls_demo: $(ALL_LIB_OBJS) build/objs/app/DtlsUdpDemo.o
	$(CXX) $(CXXFLAGS) -o build/dtls_demo $^ $(LDFLAGS)

demos: tls_demo dtls_demo
else
tls_demo dtls_demo demos:
	@echo "ERROR: $@ requires TLS=1 (mbedTLS). Re-run: make $@ TLS=1"
	@exit 1
endif

tests: \
    build/test_MessageEnvelope \
    build/test_Serializer \
    build/test_DuplicateFilter \
    build/test_ImpairmentEngine \
    build/test_LocalSim \
    build/test_AckTracker \
    build/test_RetryManager \
    build/test_DeliveryEngine \
    build/test_ImpairmentConfigLoader \
    $(TLS_TEST_BINS) \
    build/test_TcpBackend \
    build/test_UdpBackend \
    build/test_SocketUtils \
    build/test_AssertState \
    build/test_MessageId \
    build/test_Timestamp \
    build/test_RequestReplyEngine \
    build/test_Fragmentation \
    build/test_ReassemblyBuffer \
    build/test_OrderingBuffer \
    build/test_PrngEngine \
    build/test_RingBuffer \
    build/test_Logger

build/test_%: $(ALL_LIB_OBJS) build/objs/tests/test_%.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# Stress tests (separate target — NOT part of run_tests)
#
# Stress tests exercise capacity-exhaustion and slot-recycling paths with
# thousands of consecutive fill/drain cycles.  They are intentionally kept
# separate from the fast unit-test suite (run_tests) because:
#   • They take several seconds; CI fast-path should not be delayed.
#   • They target a different failure class (sustained-load slot leaks,
#     index-wrap arithmetic) that unit tests do not cover.
#
# Usage:
#   make stress_tests                      — build all four stress binaries
#   make run_stress_tests                  — build and run capacity/e2e/ordering (default 60 s each)
#   make run_stress_capacity               — capacity suite only  (default 60 s)
#   make run_stress_e2e                    — E2E suite only       (default 60 s)
#   make run_stress_ordering               — ordering suite only  (default 60 s)
#   make run_stress_reconnect              — reconnect suite only  (default 60 s; manual-trigger only in CI)
#   make run_stress_ringbuffer             — RingBuffer concurrent  (default 60 s; manual-trigger only in CI)
#   make run_stress_tcp_fanin              — TCP multi-client fan-in (default 60 s; manual-trigger only in CI)
#   make run_stress_capacity STRESS_DURATION=120  — run for 120 s instead
# ─────────────────────────────────────────────────────────────────────────────

# Duration in seconds passed to each stress binary's argv[1]. Default: 60 s.
STRESS_DURATION ?= 60

stress_tests: \
    build/test_stress_capacity \
    build/test_stress_e2e \
    build/test_stress_ordering \
    build/test_stress_reconnect

run_stress_capacity: build/test_stress_capacity
	@echo "=== Stress: capacity ($(STRESS_DURATION) s) ==="
	@build/test_stress_capacity $(STRESS_DURATION) 2>/dev/null
	@echo "=== run_stress_capacity: PASSED ==="

run_stress_e2e: build/test_stress_e2e
	@echo "=== Stress: E2E ($(STRESS_DURATION) s) ==="
	@build/test_stress_e2e $(STRESS_DURATION) 2>/dev/null
	@echo "=== run_stress_e2e: PASSED ==="

run_stress_ordering: build/test_stress_ordering
	@echo "=== Stress: ordering ($(STRESS_DURATION) s) ==="
	@build/test_stress_ordering $(STRESS_DURATION) 2>/dev/null
	@echo "=== run_stress_ordering: PASSED ==="

run_stress_tcp_fanin: build/test_stress_tcp_fanin
	@echo "=== Stress: TCP fan-in ($(STRESS_DURATION) s) ==="
	@build/test_stress_tcp_fanin $(STRESS_DURATION) 2>/dev/null
	@echo "=== run_stress_tcp_fanin: PASSED ==="

run_stress_ringbuffer: build/test_stress_ringbuffer
	@echo "=== Stress: RingBuffer concurrent ($(STRESS_DURATION) s) ==="
	@build/test_stress_ringbuffer $(STRESS_DURATION) 2>/dev/null
	@echo "=== run_stress_ringbuffer: PASSED ==="

run_stress_reconnect: build/test_stress_reconnect
	@echo "=== Stress: reconnect ($(STRESS_DURATION) s) ==="
	@build/test_stress_reconnect $(STRESS_DURATION) 2>/dev/null
	@echo "=== run_stress_reconnect: PASSED ==="

run_stress_tests: stress_tests
	@$(MAKE) run_stress_capacity  STRESS_DURATION=$(STRESS_DURATION)
	@$(MAKE) run_stress_e2e       STRESS_DURATION=$(STRESS_DURATION)
	@$(MAKE) run_stress_ordering  STRESS_DURATION=$(STRESS_DURATION)
	@echo "=== STRESS TESTS PASSED (reconnect suite excluded — run separately: make run_stress_reconnect) ==="

# ─────────────────────────────────────────────────────────────────────────────
# Sanitizer tests (ASan + UBSan) — separate target, same test suite as run_tests
#
# Runs the complete unit-test suite compiled with AddressSanitizer and
# UndefinedBehaviorSanitizer.  Catches buffer overflows, use-after-free,
# signed integer overflow, misaligned access, invalid enum values, and other
# MISRA C++:2023 / Power of 10 UB classes that the compiler and static
# analysis tools do not always flag.
#
# Binaries land in build/san/ (separate from build/test_* to allow both to
# coexist without a clean).  Object files land in build/san-objs/.
#
# Usage:
#   make sanitize_tests   — build only
#   make run_sanitize     — build and run (recommended)
#
# Portability:
#   macOS  — Apple clang 17; LSan absent; no ASAN_OPTIONS override needed.
#   Linux  — GCC or clang; LSan active by default; ASAN_OPTIONS=detect_leaks=0
#             suppresses mbedTLS false positives (see variable definition above).
# ─────────────────────────────────────────────────────────────────────────────
sanitize_tests: \
    build/san/test_MessageEnvelope \
    build/san/test_Serializer \
    build/san/test_DuplicateFilter \
    build/san/test_ImpairmentEngine \
    build/san/test_LocalSim \
    build/san/test_AckTracker \
    build/san/test_RetryManager \
    build/san/test_DeliveryEngine \
    build/san/test_ImpairmentConfigLoader \
    $(TLS_SAN_BINS) \
    build/san/test_TcpBackend \
    build/san/test_UdpBackend \
    build/san/test_SocketUtils \
    build/san/test_AssertState \
    build/san/test_MessageId \
    build/san/test_Timestamp \
    build/san/test_RequestReplyEngine \
    build/san/test_Fragmentation \
    build/san/test_ReassemblyBuffer \
    build/san/test_OrderingBuffer \
    build/san/test_PrngEngine \
    build/san/test_RingBuffer \
    build/san/test_Logger

run_sanitize: sanitize_tests
	@echo "=== Sanitizer tests: ASan + UBSan ($(if $(SAN_RUN_ENV),$(SAN_RUN_ENV),no LSan suppression needed)) ==="
	@$(SAN_RUN_ENV) build/san/test_MessageEnvelope
	@$(SAN_RUN_ENV) build/san/test_Serializer
	@$(SAN_RUN_ENV) build/san/test_DuplicateFilter
	@$(SAN_RUN_ENV) build/san/test_ImpairmentEngine
	@$(SAN_RUN_ENV) build/san/test_LocalSim
	@$(SAN_RUN_ENV) build/san/test_AckTracker
	@$(SAN_RUN_ENV) build/san/test_RetryManager
	@$(SAN_RUN_ENV) build/san/test_DeliveryEngine
	@$(SAN_RUN_ENV) build/san/test_ImpairmentConfigLoader
	@if [ "$(TLS)" = "1" ]; then $(SAN_RUN_ENV) build/san/test_TlsTcpBackend; fi
	@if [ "$(TLS)" = "1" ]; then $(SAN_RUN_ENV) build/san/test_DtlsUdpBackend; fi
	@$(SAN_RUN_ENV) build/san/test_TcpBackend
	@$(SAN_RUN_ENV) build/san/test_UdpBackend
	@$(SAN_RUN_ENV) build/san/test_SocketUtils
	@$(SAN_RUN_ENV) build/san/test_AssertState
	@$(SAN_RUN_ENV) build/san/test_MessageId
	@$(SAN_RUN_ENV) build/san/test_Timestamp
	@$(SAN_RUN_ENV) build/san/test_RequestReplyEngine
	@$(SAN_RUN_ENV) build/san/test_Fragmentation
	@$(SAN_RUN_ENV) build/san/test_ReassemblyBuffer
	@$(SAN_RUN_ENV) build/san/test_OrderingBuffer
	@$(SAN_RUN_ENV) build/san/test_PrngEngine
	@$(SAN_RUN_ENV) build/san/test_RingBuffer
	@$(SAN_RUN_ENV) build/san/test_Logger
	@echo "$(RUN_SANITIZE_FOOTER)"

build/san/test_%: $(SAN_LIB_OBJS) $(SAN_OBJ_DIR)/tests/test_%.o
	@mkdir -p build/san
	$(CXX) $(SAN_CXXFLAGS) -o $@ $^ $(SAN_LDFLAGS)

$(SAN_OBJ_DIR)/core/%.o: src/core/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) -c -o $@ $<

$(SAN_OBJ_DIR)/platform/%.o: src/platform/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) -c -o $@ $<

$(SAN_OBJ_DIR)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SAN_CXXFLAGS) -c -o $@ $<

# ─────────────────────────────────────────────────────────────────────────────
# Compile rules
# ─────────────────────────────────────────────────────────────────────────────
$(BUILD_DIR)/core/%.o: src/core/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/platform/%.o: src/platform/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/app/%.o: src/app/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ─────────────────────────────────────────────────────────────────────────────
# check_version — pre-release gate
#
# Verifies that the ME_VERSION_STRING in src/core/Version.hpp matches the
# current git tag exactly (format: vMAJOR.MINOR.PATCH).  Run this before
# creating a GitHub Release to catch the common mistake of bumping one but
# forgetting the other.
#
# Exits 0 (success) when the current HEAD is exactly at a tag that matches
# the version string.  Exits 1 with a clear message otherwise.
#
# Usage:
#   make check_version
# ─────────────────────────────────────────────────────────────────────────────
check_version:
	@tag=$$(git describe --tags --exact-match 2>/dev/null || echo "untagged"); \
	 src=$$(grep 'ME_VERSION_STRING\[\]' src/core/Version.hpp \
	        | sed 's/.*"\(.*\)".*/\1/'); \
	 if [ "$$tag" = "v$$src" ]; then \
	     echo "=== check_version: OK — tag=$$tag matches src/core/Version.hpp ($$src) ==="; \
	 else \
	     echo ""; \
	     echo "ERROR: Version mismatch"; \
	     echo "  git tag              : $$tag"; \
	     echo "  src/core/Version.hpp : $$src"; \
	     echo ""; \
	     echo "Fix: ensure ME_VERSION_STRING in Version.hpp equals the tag (without 'v'),"; \
	     echo "     commit, then re-tag before running make check_version."; \
	     echo ""; \
	     exit 1; \
	 fi

check_traceability:
	@bash docs/check_traceability.sh

# ─────────────────────────────────────────────────────────────────────────────
# Static analysis targets (CLAUDE.md §9.1)
# Tier 2: Clang-Tidy and Cppcheck — run on every change before merge.
# Tier 3: PC-lint Plus             — run before any release or formal review.
# ─────────────────────────────────────────────────────────────────────────────

# Tier 2a: Clang-Tidy
# Config: src/.clang-tidy (strict), tests/.clang-tidy (relaxed).
# clang-tidy discovers the nearest .clang-tidy by searching up from each source file.
lint:
	@echo "=== Clang-Tidy: src/ (strict) ==="
	@$(CLANG_TIDY) $(filter-out $(TLS_LINT_EXCL_SRC),$(shell find src -name '*.cpp')) -- $(CXXFLAGS)
	@echo "=== Clang-Tidy: tests/ (relaxed) ==="
	@$(CLANG_TIDY) $(filter-out $(TLS_LINT_EXCL_TEST),$(shell find tests -name '*.cpp')) -- $(CXXFLAGS)
	@echo "=== Clang-Tidy: PASS ==="

# Tier 2b: Cppcheck flow/style analysis (CI-safe; no addon required).
# MISRA C++:2023 enforcement is covered by clang-tidy (Tier 2a, above).
# Config: .cppcheck-suppress (documented deviation suppressions).
#       On macOS: brew install cppcheck
#       On Linux: apt install cppcheck
cppcheck:
	@echo "=== Cppcheck: src/ ==="
	@grep -v '^#' .cppcheck-suppress | grep -v '^$$' > /tmp/.cppcheck-supp.$$$$.tmp && \
	 cppcheck --enable=all --error-exitcode=1 \
	    --suppress=checkersReport \
	    --suppressions-list=/tmp/.cppcheck-supp.$$$$.tmp \
	    --std=c++17 -I src \
	    $(CPPCHECK_TLS_EXCL) \
	    src/ 2>&1
	@echo "=== Cppcheck: PASS ==="

# Tier 2b (extended): Cppcheck + MISRA C++:2023 addon (local use only).
# cppcheck 2.13 (Ubuntu 24.04) fails when --addon=misra is combined with
# --suppressions-list; the addon is therefore excluded from the CI target.
# Run this target locally on macOS (brew install cppcheck includes misra.py).
#       On macOS: brew install cppcheck
#       On Linux: apt install cppcheck  (addon disabled; use make cppcheck instead)
cppcheck-misra:
	@echo "=== Cppcheck + MISRA C++:2023 addon: src/ ==="
	@grep -v '^#' .cppcheck-suppress | grep -v '^$$' | \
	    cppcheck --enable=all --error-exitcode=1 \
	    --addon=misra \
	    --suppressions-list=/dev/stdin \
	    --std=c++17 -I src \
	    $(CPPCHECK_TLS_EXCL) \
	    src/ 2>&1
	@echo "=== Cppcheck + MISRA addon: PASS ==="

# Tier 3: PC-lint Plus formal MISRA C++:2023 compliance report
# TODO: install PC-lint Plus and create pclint/ config directory.
pclint:
	@echo "=== PC-lint Plus: MISRA C++:2023 compliance report ==="
	@lint-nt pclint/co-gcc.lnt pclint/misra_cpp_2023.lnt \
	    $(shell find src -name '*.cpp') 2>&1

# Tier 2c: Clang Static Analyzer (scan-build)
# Wraps compilation via c++-analyzer interceptor for path-sensitive bug detection.
# --status-bugs: exits non-zero when any checker finding is reported.
# Alpha checkers are selectively enabled for safety-critical (Power of 10) analysis.
# -Werror is stripped so clang dialect differences do not mask real findings.
# Artifacts land in build/scan-build/ (HTML reports) and build/scan-objs/ (objects).
SCAN_BUILD    := $(if $(LLVM_BIN),$(LLVM_BIN)/scan-build,scan-build)
SCAN_ANALYZER := $(if $(LLVM_BIN),$(LLVM_BIN)/clang,$(shell which clang 2>/dev/null))
CXX_ANALYZER  := $(wildcard /opt/homebrew/opt/llvm/libexec/c++-analyzer)
SCAN_OBJ_DIR  := build/scan-objs
SCAN_CXXFLAGS := $(filter-out -Werror,$(CXXFLAGS))
SCAN_SRCS     := $(CORE_SRC) $(PLATFORM_SRC) src/app/Server.cpp src/app/Client.cpp
SCAN_OBJS     := $(patsubst src/%.cpp,$(SCAN_OBJ_DIR)/%.o,$(SCAN_SRCS))

$(SCAN_OBJ_DIR)/core/%.o: src/core/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SCAN_CXXFLAGS) -c -o $@ $<

$(SCAN_OBJ_DIR)/platform/%.o: src/platform/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SCAN_CXXFLAGS) -c -o $@ $<

$(SCAN_OBJ_DIR)/app/%.o: src/app/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(SCAN_CXXFLAGS) -c -o $@ $<

scan_build:
	@echo "=== Clang Static Analyzer: src/ ==="
	@echo "=== scan-build=$(SCAN_BUILD)  analyzer=$(SCAN_ANALYZER)$(if $(CXX_ANALYZER),  CXX_ANALYZER=$(CXX_ANALYZER),) ==="
	@$(SCAN_BUILD) \
	    --use-analyzer=$(SCAN_ANALYZER) \
	    --status-bugs \
	    -o build/scan-build \
	    -enable-checker alpha.core.BoolAssignment \
	    -enable-checker alpha.core.Conversion \
	    -enable-checker alpha.unix.cstring.OutOfBounds \
	    $(MAKE) $(SCAN_OBJS) \
	        $(if $(CXX_ANALYZER),CXX="$(CXX_ANALYZER)",) \
	        CXXFLAGS="$(SCAN_CXXFLAGS)"
	@echo "=== Clang Static Analyzer: PASS ==="

# Umbrella: Tier 2 tools in sequence (use this in CI)
static_analysis: lint cppcheck scan_build

# ─────────────────────────────────────────────────────────────────────────────
# Coverage analysis — LLVM source-based coverage (CLAUDE.md §12)
# Policy: SC functions (docs/HAZARD_ANALYSIS.md §3) require ≥ branch coverage.
#         MC/DC is the goal for the five highest-hazard functions:
#           DeliveryEngine::send, DeliveryEngine::receive,
#           DuplicateFilter::check_and_record, Serializer::serialize,
#           Serializer::deserialize.
# Uses clang++ -fprofile-instr-generate -fcoverage-mapping; no lcov required.
# Artifacts land in build/cov/ (raw profiles, merged data) and build/cov-objs/.
# ─────────────────────────────────────────────────────────────────────────────
COV_CXX       := $(if $(LLVM_BIN),$(LLVM_BIN)/clang++,clang++)
LLVM_PROFDATA := $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-profdata,llvm-profdata)
LLVM_COV      := $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-cov,llvm-cov)
COV_OBJ_DIR   := build/cov-objs
# Strip -Werror so clang++ dialect differences do not block coverage builds;
# add instrumentation flags; disable optimisation for accurate line mapping.
COV_CXXFLAGS  := $(filter-out -Werror,$(CXXFLAGS)) \
                 -fprofile-instr-generate -fcoverage-mapping -O0
COV_LDFLAGS   := -fprofile-instr-generate $(LDFLAGS)
COV_LIB_OBJS  := $(patsubst src/%.cpp,$(COV_OBJ_DIR)/%.o,$(ALL_LIB_SRC))
TEST_NAMES_BASE := MessageEnvelope Serializer DuplicateFilter ImpairmentEngine LocalSim AckTracker RetryManager DeliveryEngine ImpairmentConfigLoader TcpBackend UdpBackend SocketUtils AssertState MessageId Timestamp RequestReplyEngine Fragmentation ReassemblyBuffer OrderingBuffer RingBuffer PrngEngine Logger
TEST_NAMES      := $(TEST_NAMES_BASE) $(TLS_TEST_NAMES)

# TLS-conditional profraw and object-flag fragments for llvm-profdata / llvm-cov.
ifeq ($(TLS),1)
COV_TLS_PROFRAW := build/cov/TlsTcpBackend.profraw \
                   build/cov/DtlsUdpBackend.profraw
COV_TLS_OBJECTS := -object build/cov_test_TlsTcpBackend \
                   -object build/cov_test_DtlsUdpBackend
else
COV_TLS_PROFRAW :=
COV_TLS_OBJECTS :=
endif
COV_TESTS     := $(patsubst %,build/cov_test_%,$(TEST_NAMES))

$(COV_OBJ_DIR)/core/%.o: src/core/%.cpp
	@mkdir -p $(dir $@)
	$(COV_CXX) $(COV_CXXFLAGS) -c -o $@ $<

$(COV_OBJ_DIR)/platform/%.o: src/platform/%.cpp
	@mkdir -p $(dir $@)
	$(COV_CXX) $(COV_CXXFLAGS) -c -o $@ $<

$(COV_OBJ_DIR)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(COV_CXX) $(COV_CXXFLAGS) -c -o $@ $<

build/cov_test_%: $(COV_LIB_OBJS) $(COV_OBJ_DIR)/tests/test_%.o
	$(COV_CXX) $(COV_CXXFLAGS) -o $@ $^ $(COV_LDFLAGS)

coverage: $(COV_TESTS)
	@echo "=== Coverage: running instrumented tests ==="
	@mkdir -p build/cov
	@LLVM_PROFILE_FILE="build/cov/MessageEnvelope.profraw" build/cov_test_MessageEnvelope >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/Serializer.profraw"      build/cov_test_Serializer      >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/DuplicateFilter.profraw" build/cov_test_DuplicateFilter >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/ImpairmentEngine.profraw" build/cov_test_ImpairmentEngine >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/LocalSim.profraw"        build/cov_test_LocalSim        >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/AckTracker.profraw"    build/cov_test_AckTracker    >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/RetryManager.profraw"  build/cov_test_RetryManager  >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/DeliveryEngine.profraw" build/cov_test_DeliveryEngine >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/ImpairmentConfigLoader.profraw" build/cov_test_ImpairmentConfigLoader >/dev/null 2>&1
	@if [ "$(TLS)" = "1" ]; then LLVM_PROFILE_FILE="build/cov/TlsTcpBackend.profraw" build/cov_test_TlsTcpBackend >/dev/null 2>&1; fi
	@if [ "$(TLS)" = "1" ]; then LLVM_PROFILE_FILE="build/cov/DtlsUdpBackend.profraw" build/cov_test_DtlsUdpBackend >/dev/null 2>&1; fi
	@LLVM_PROFILE_FILE="build/cov/TcpBackend.profraw"    build/cov_test_TcpBackend    >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/UdpBackend.profraw"    build/cov_test_UdpBackend    >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/SocketUtils.profraw"  build/cov_test_SocketUtils   >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/AssertState.profraw"  build/cov_test_AssertState   >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/MessageId.profraw"   build/cov_test_MessageId     >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/Timestamp.profraw"   build/cov_test_Timestamp     >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/RequestReplyEngine.profraw" build/cov_test_RequestReplyEngine >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/Fragmentation.profraw" build/cov_test_Fragmentation >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/ReassemblyBuffer.profraw" build/cov_test_ReassemblyBuffer >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/OrderingBuffer.profraw" build/cov_test_OrderingBuffer >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/RingBuffer.profraw"  build/cov_test_RingBuffer    >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/PrngEngine.profraw"  build/cov_test_PrngEngine    >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/Logger.profraw"      build/cov_test_Logger        >/dev/null 2>&1
	@$(LLVM_PROFDATA) merge -sparse \
	    build/cov/MessageEnvelope.profraw \
	    build/cov/Serializer.profraw \
	    build/cov/DuplicateFilter.profraw \
	    build/cov/ImpairmentEngine.profraw \
	    build/cov/LocalSim.profraw \
	    build/cov/AckTracker.profraw \
	    build/cov/RetryManager.profraw \
	    build/cov/DeliveryEngine.profraw \
	    build/cov/ImpairmentConfigLoader.profraw \
	    $(COV_TLS_PROFRAW) \
	    build/cov/TcpBackend.profraw \
	    build/cov/UdpBackend.profraw \
	    build/cov/SocketUtils.profraw \
	    build/cov/AssertState.profraw \
	    build/cov/MessageId.profraw \
	    build/cov/Timestamp.profraw \
	    build/cov/RequestReplyEngine.profraw \
	    build/cov/Fragmentation.profraw \
	    build/cov/ReassemblyBuffer.profraw \
	    build/cov/OrderingBuffer.profraw \
	    build/cov/RingBuffer.profraw \
	    build/cov/PrngEngine.profraw \
	    build/cov/Logger.profraw \
	    -o build/cov/merged.profdata
	@echo "=== Coverage Report (src/ only) ==="
	@$(LLVM_COV) report \
	    -instr-profile=build/cov/merged.profdata \
	    build/cov_test_MessageEnvelope \
	    -object build/cov_test_Serializer \
	    -object build/cov_test_DuplicateFilter \
	    -object build/cov_test_ImpairmentEngine \
	    -object build/cov_test_LocalSim \
	    -object build/cov_test_AckTracker \
	    -object build/cov_test_RetryManager \
	    -object build/cov_test_DeliveryEngine \
	    -object build/cov_test_ImpairmentConfigLoader \
	    $(COV_TLS_OBJECTS) \
	    -object build/cov_test_TcpBackend \
	    -object build/cov_test_UdpBackend \
	    -object build/cov_test_SocketUtils \
	    -object build/cov_test_AssertState \
	    -object build/cov_test_MessageId \
	    -object build/cov_test_Timestamp \
	    -object build/cov_test_RequestReplyEngine \
	    -object build/cov_test_Fragmentation \
	    -object build/cov_test_ReassemblyBuffer \
	    -object build/cov_test_OrderingBuffer \
	    -object build/cov_test_RingBuffer \
	    -object build/cov_test_PrngEngine \
	    -object build/cov_test_Logger \
	    $(CORE_SRC) $(PLATFORM_SRC)
	@echo ""
	@echo "Policy (CLAUDE.md §12): SC functions require >= branch coverage."
	@echo "  Run 'make coverage_show' for annotated line-level output."
	@echo "=== Coverage: DONE ==="

# coverage_show: line-level annotated output; requires 'make coverage' first.
coverage_show:
	@$(LLVM_COV) show \
	    -instr-profile=build/cov/merged.profdata \
	    build/cov_test_MessageEnvelope \
	    -object build/cov_test_Serializer \
	    -object build/cov_test_DuplicateFilter \
	    -object build/cov_test_ImpairmentEngine \
	    -object build/cov_test_LocalSim \
	    -object build/cov_test_AckTracker \
	    -object build/cov_test_RetryManager \
	    -object build/cov_test_DeliveryEngine \
	    -object build/cov_test_ImpairmentConfigLoader \
	    $(COV_TLS_OBJECTS) \
	    -object build/cov_test_TcpBackend \
	    -object build/cov_test_UdpBackend \
	    -object build/cov_test_SocketUtils \
	    -object build/cov_test_AssertState \
	    -object build/cov_test_MessageId \
	    -object build/cov_test_Timestamp \
	    -object build/cov_test_RequestReplyEngine \
	    -object build/cov_test_Fragmentation \
	    -object build/cov_test_ReassemblyBuffer \
	    -object build/cov_test_OrderingBuffer \
	    -object build/cov_test_RingBuffer \
	    -object build/cov_test_PrngEngine \
	    -format=text \
	    -show-branches=count \
	    $(CORE_SRC) $(PLATFORM_SRC)

# coverage_report: formatted per-file + per-function report with policy compliance notes.
# Depends on 'coverage' so profiles are always up to date before reporting.
# Per-file percentages come live from llvm-cov; policy annotations are fixed facts.
coverage_report: coverage
	@echo ""
	@echo "================================================================"
	@echo "   messageEngine — Test Coverage Report"
	@echo "   $$(date '+%Y-%m-%d %H:%M') | LLVM source-based coverage"
	@echo "================================================================"
	@echo ""
	@echo "--- Per-file summary (functions / lines / branches) ---"
	@$(LLVM_COV) report \
	    -instr-profile=build/cov/merged.profdata \
	    build/cov_test_MessageEnvelope \
	    -object build/cov_test_Serializer \
	    -object build/cov_test_DuplicateFilter \
	    -object build/cov_test_ImpairmentEngine \
	    -object build/cov_test_LocalSim \
	    -object build/cov_test_AckTracker \
	    -object build/cov_test_RetryManager \
	    -object build/cov_test_DeliveryEngine \
	    -object build/cov_test_ImpairmentConfigLoader \
	    $(COV_TLS_OBJECTS) \
	    -object build/cov_test_TcpBackend \
	    -object build/cov_test_UdpBackend \
	    -object build/cov_test_SocketUtils \
	    -object build/cov_test_AssertState \
	    -object build/cov_test_MessageId \
	    -object build/cov_test_Timestamp \
	    -object build/cov_test_RequestReplyEngine \
	    -object build/cov_test_Fragmentation \
	    -object build/cov_test_ReassemblyBuffer \
	    -object build/cov_test_OrderingBuffer \
	    -object build/cov_test_RingBuffer \
	    -object build/cov_test_PrngEngine \
	    -object build/cov_test_Logger \
	    $(CORE_SRC) $(PLATFORM_SRC)
	@echo ""
	@echo "--- Per-function detail ---"
	@$(LLVM_COV) report \
	    --show-functions \
	    -instr-profile=build/cov/merged.profdata \
	    build/cov_test_MessageEnvelope \
	    -object build/cov_test_Serializer \
	    -object build/cov_test_DuplicateFilter \
	    -object build/cov_test_ImpairmentEngine \
	    -object build/cov_test_LocalSim \
	    -object build/cov_test_AckTracker \
	    -object build/cov_test_RetryManager \
	    -object build/cov_test_DeliveryEngine \
	    -object build/cov_test_ImpairmentConfigLoader \
	    $(COV_TLS_OBJECTS) \
	    -object build/cov_test_TcpBackend \
	    -object build/cov_test_UdpBackend \
	    -object build/cov_test_SocketUtils \
	    -object build/cov_test_AssertState \
	    -object build/cov_test_MessageId \
	    -object build/cov_test_Timestamp \
	    -object build/cov_test_RequestReplyEngine \
	    -object build/cov_test_Fragmentation \
	    -object build/cov_test_ReassemblyBuffer \
	    -object build/cov_test_OrderingBuffer \
	    -object build/cov_test_RingBuffer \
	    -object build/cov_test_PrngEngine \
	    -object build/cov_test_Logger \
	    $(CORE_SRC) $(PLATFORM_SRC)
	@echo ""
	@echo "--- Policy compliance (CLAUDE.md §14) ---"
	@echo "  Floor: >= 75% branch coverage for all SC function files."
	@echo ""
	@echo "  Documented ceilings (max achievable -- not defects):"
	@echo "    core/Serializer.cpp              74% ceiling"
	@echo "      20 permanently-missed branches: 1 per NEVER_COMPILED_OUT_ASSERT"
	@echo "      ([[noreturn]] abort path; LLVM skips its profile counter)."
	@echo "    platform/ImpairmentEngine.cpp    74% ceiling"
	@echo "      48 missed: 40 assert branches + 8 architecturally-impossible"
	@echo "      logic branches (assert-protected or mathematically unreachable)."
	@echo ""
	@echo "    platform/TcpBackend.cpp         78% ceiling"
	@echo "      49 missed: 39 assert True branches + ~10 architecturally-unreachable"
	@echo "      branches (poll_clients_once POLLIN accept, partial-read loop,"
	@echo "      serialize fail). 4 new branches covered: remove_client_fd False@idx0,"
	@echo "      recv_queue overflow, send_frame fail, loss-impairment ERR_IO drop."
	@echo ""
	@echo "    platform/UdpBackend.cpp         75% ceiling"
	@echo "      24 missed: 19 assert True branches + ~5 architecturally-unreachable"
	@echo "      branches (recv_one_datagram second-datagram poll, serialize fail,"
	@echo "      recv_queue full unreachable: max depth 31 < MSG_RING_CAPACITY 64)."
	@echo "      3 new branches covered: loss impairment, send_to fail, initial pop."
	@echo ""
	@echo "    platform/DtlsUdpBackend.cpp     81% ceiling"
	@echo "      45 missed: 24 assert True branches + ~21 hard mbedTLS/structural"
	@echo "      paths (ssl_write, DTLS handshake limit, recv_queue full, WANT_READ)."
	@echo "      6 new branches covered: flush_delayed loop, post-flush pop,"
	@echo "      loss-impairment ERR_IO, num_channels==0 init (via new Option A tests)."
	@echo ""
	@echo "    platform/ImpairmentConfigLoader.cpp 90% ceiling"
	@echo "      10 missed: 5 assert True branches (apply_kv ×2, parse_config_line ×2,"
	@echo "      impairment_config_load ×1) + 4 compound-assert sub-branches (loss/"
	@echo "      dup probability postcondition &&-guards, always-True after clamping)"
	@echo "      + 1 fclose() failure path (unreachable in non-adversarial test env)."
	@echo ""
	@echo "    platform/SocketUtils.cpp    64% ceiling"
	@echo "      83 missed: ~19 POSIX hard error paths unreachable on loopback"
	@echo "      (fcntl F_GETFL/F_SETFL, setsockopt, listen, accept, close,"
	@echo "      recvfrom, inet_ntop) + UDP partial-send atomicity assumption"
	@echo "      (send_result != len never occurs for loopback datagrams)."
	@echo "      All 2 newly-reachable branches covered (inet_aton failure in"
	@echo "      socket_bind and socket_send_to via invalid-IP unit tests)."
	@echo ""
	@echo "    core/AssertState.cpp        100% (no branches to instrument;"
	@echo "      check_and_clear atomic op has no LLVM branch points; all"
	@echo "      7 lines covered by test_AssertState.cpp)."
	@echo ""
	@echo "    platform/LocalSimHarness.cpp    72% ceiling"
	@echo "      19 missed: 17 assert True branches + 2 structurally-unreachable"
	@echo "      (L165 True >5s cap, L179 True single-thread). Dead ternary at"
	@echo "      old L164 eliminated by P2 dead-code removal (71→69 branches)."
	@echo "      Loss path and delay loop now covered via Option A (VVP-001 M5)."
	@echo ""
	@echo "  NSC files (line coverage sufficient, branch floor not enforced):"
	@echo "    platform/LocalSimHarness.cpp   (NSC — ceiling documented above)"
	@echo "    platform/SocketUtils.cpp    (NSC — ceiling documented above)"
	@echo "    core/AssertState.cpp        (NSC — 100% lines covered)"
	@echo ""
	@echo "  See CLAUDE.md §14 for full policy and ceiling justifications."
	@echo "================================================================"

# ─────────────────────────────────────────────────────────────────────────────
# PIC object compilation rule (shared library only).
# Power of 10 Rule 2 deviation: infrastructure pattern rule — bounded by
# ALL_PIC_LIB_OBJS which is derived from the finite ALL_LIB_SRC list.
# ─────────────────────────────────────────────────────────────────────────────
$(PIC_OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(PIC_CXXFLAGS) -c -o $@ $<

# ─────────────────────────────────────────────────────────────────────────────
# static_lib — archives all library objects into libmessageengine.a.
# Reuses the existing non-PIC build/objs/ objects (fine for static linking).
# ─────────────────────────────────────────────────────────────────────────────
static_lib: $(ALL_LIB_OBJS)
	ar rcs build/libmessageengine.a $^
	@echo "=== static_lib: build/libmessageengine.a ==="

# ─────────────────────────────────────────────────────────────────────────────
# shared_lib — links PIC objects into a versioned shared library.
# Platform-aware: .so on Linux, .dylib on macOS.
# ─────────────────────────────────────────────────────────────────────────────
shared_lib: $(ALL_PIC_LIB_OBJS)
	$(CXX) $(SHARED_LDFLAGS) -o $(SHARED_LIB) $^ $(LDFLAGS)
	ln -sf $(notdir $(SHARED_LIB)) $(SHARED_SYMLINK1)
	ln -sf $(notdir $(SHARED_SYMLINK1)) $(SHARED_SYMLINK2)
	@echo "=== shared_lib: $(SHARED_LIB) ==="

# ─────────────────────────────────────────────────────────────────────────────
# libs — builds both static and shared libraries.
# ─────────────────────────────────────────────────────────────────────────────
libs: static_lib shared_lib

# ─────────────────────────────────────────────────────────────────────────────
# install-dev — installs library headers + static lib + shared lib + pkg-config
# into $(DESTDIR)$(includedir) and $(DESTDIR)$(libdir).
# Yocto do_install calls: make install-dev DESTDIR=${D} prefix=${prefix}
# ─────────────────────────────────────────────────────────────────────────────
install-dev: libs
	install -d $(DESTDIR)$(includedir)/messageengine/testing
	install -m 0644 $(PUBLIC_HEADERS_CORE) $(PUBLIC_HEADERS_PLATFORM) \
	    $(DESTDIR)$(includedir)/messageengine/
	install -m 0644 $(TESTING_HEADERS) \
	    $(DESTDIR)$(includedir)/messageengine/testing/
	install -d $(DESTDIR)$(libdir)/pkgconfig
	install -m 0644 build/libmessageengine.a $(DESTDIR)$(libdir)/
	install -m 0755 $(SHARED_LIB) $(DESTDIR)$(libdir)/
	ln -sf $(notdir $(SHARED_LIB)) \
	    $(DESTDIR)$(libdir)/$(notdir $(SHARED_SYMLINK1))
	ln -sf $(notdir $(SHARED_SYMLINK1)) \
	    $(DESTDIR)$(libdir)/$(notdir $(SHARED_SYMLINK2))
	sed -e 's|@PREFIX@|$(DESTDIR)$(prefix)|g' \
	    -e 's|@LIBDIR@|$(DESTDIR)$(libdir)|g' \
	    -e 's|@INCLUDEDIR@|$(DESTDIR)$(includedir)|g' \
	    -e 's|@VERSION@|$(ME_VERSION)|g' \
	    packaging/messageengine.pc.in \
	    > $(DESTDIR)$(libdir)/pkgconfig/messageengine.pc
	@echo "=== install-dev: headers + libs installed to $(DESTDIR)$(prefix) ==="

# ─────────────────────────────────────────────────────────────────────────────
# package — builds a self-contained distributable tarball.
# Output: build/messageengine-vVERSION-OS-ARCH.tar.gz
# Does NOT require a matching git tag (use check_version + release job for that).
# ─────────────────────────────────────────────────────────────────────────────
PACKAGE_OS   := $(shell uname -s | tr '[:upper:]' '[:lower:]' | sed 's/darwin/darwin/')
PACKAGE_ARCH := $(shell uname -m)
PACKAGE_NAME := messageengine-v$(ME_VERSION)-$(PACKAGE_OS)-$(PACKAGE_ARCH)
STAGE        := build/pkg/$(PACKAGE_NAME)

package: libs
	@echo "=== Staging package: $(PACKAGE_NAME) ==="
	rm -rf $(STAGE)
	mkdir -p $(STAGE)/include/messageengine/testing
	mkdir -p $(STAGE)/lib/pkgconfig
	mkdir -p $(STAGE)/lib/cmake/MessageEngine
	install -m 0644 $(PUBLIC_HEADERS_CORE) $(PUBLIC_HEADERS_PLATFORM) \
	    $(STAGE)/include/messageengine/
	install -m 0644 $(TESTING_HEADERS) \
	    $(STAGE)/include/messageengine/testing/
	install -m 0644 build/libmessageengine.a $(STAGE)/lib/
	install -m 0755 $(SHARED_LIB) $(STAGE)/lib/
	ln -sf $(notdir $(SHARED_LIB)) \
	    $(STAGE)/lib/$(notdir $(SHARED_SYMLINK1))
	ln -sf $(notdir $(SHARED_SYMLINK1)) \
	    $(STAGE)/lib/$(notdir $(SHARED_SYMLINK2))
	sed -e 's|@PREFIX@|/usr|g' \
	    -e 's|@LIBDIR@|/usr/lib|g' \
	    -e 's|@INCLUDEDIR@|/usr/include|g' \
	    -e 's|@VERSION@|$(ME_VERSION)|g' \
	    packaging/messageengine.pc.in \
	    > $(STAGE)/lib/pkgconfig/messageengine.pc
	sed 's|@VERSION@|$(ME_VERSION)|g' \
	    packaging/cmake/MessageEngineConfig.cmake.in \
	    > $(STAGE)/lib/cmake/MessageEngine/MessageEngineConfig.cmake
	sed 's|@VERSION@|$(ME_VERSION)|g' \
	    packaging/cmake/MessageEngineConfigVersion.cmake.in \
	    > $(STAGE)/lib/cmake/MessageEngine/MessageEngineConfigVersion.cmake
	sed -e 's|@VERSION@|$(ME_VERSION)|g' \
	    -e 's|@SHARED_LIB_NAME@|$(notdir $(SHARED_LIB))|g' \
	    packaging/cmake/MessageEngineTargets.cmake.in \
	    > $(STAGE)/lib/cmake/MessageEngine/MessageEngineTargets.cmake
	install -m 0644 LICENSE $(STAGE)/
	tar -czf build/$(PACKAGE_NAME).tar.gz -C build/pkg $(PACKAGE_NAME)
	@echo "=== Package ready: build/$(PACKAGE_NAME).tar.gz ==="

# ─────────────────────────────────────────────────────────────────────────────
# install — Yocto do_install calls: make install DESTDIR=${D}
# Installs server and client binaries to $(DESTDIR)$(bindir).
# Build must complete first: make server client && make install DESTDIR=...
# ─────────────────────────────────────────────────────────────────────────────
install: server client
	install -d $(DESTDIR)$(bindir)
	install -m 0755 build/server $(DESTDIR)$(bindir)/me-server
	install -m 0755 build/client $(DESTDIR)$(bindir)/me-client

clean:
	rm -rf build/

run_tests: tests
	@echo "=== test_MessageEnvelope ==="; build/test_MessageEnvelope
	@echo "=== test_Serializer ===";      build/test_Serializer
	@echo "=== test_DuplicateFilter ==="; build/test_DuplicateFilter
	@echo "=== test_ImpairmentEngine ==="; build/test_ImpairmentEngine
	@echo "=== test_LocalSim ===";        build/test_LocalSim
	@echo "=== test_AckTracker ===";    build/test_AckTracker
	@echo "=== test_RetryManager ===";  build/test_RetryManager
	@echo "=== test_DeliveryEngine ==="; build/test_DeliveryEngine
	@echo "=== test_ImpairmentConfigLoader ==="; build/test_ImpairmentConfigLoader
	@if [ "$(TLS)" = "1" ]; then echo "=== test_TlsTcpBackend ==="; build/test_TlsTcpBackend; fi
	@if [ "$(TLS)" = "1" ]; then echo "=== test_DtlsUdpBackend ==="; build/test_DtlsUdpBackend; fi
	@echo "=== test_TcpBackend ==="; build/test_TcpBackend
	@echo "=== test_UdpBackend ==="; build/test_UdpBackend
	@echo "=== test_SocketUtils ==="; build/test_SocketUtils
	@echo "=== test_AssertState ==="; build/test_AssertState
	@echo "=== test_MessageId ===";  build/test_MessageId
	@echo "=== test_Timestamp ===";  build/test_Timestamp
	@echo "=== test_RequestReplyEngine ==="; build/test_RequestReplyEngine
	@echo "=== test_Fragmentation ==="; build/test_Fragmentation
	@echo "=== test_ReassemblyBuffer ==="; build/test_ReassemblyBuffer
	@echo "=== test_OrderingBuffer ==="; build/test_OrderingBuffer
	@echo "=== test_PrngEngine ==="; build/test_PrngEngine
	@echo "=== test_RingBuffer ==="; build/test_RingBuffer
	@echo "=== test_Logger ==="; build/test_Logger
	@echo "$(RUN_TESTS_FOOTER)"
