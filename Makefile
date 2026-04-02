# Detect OS
UNAME_S := $(shell uname -s)

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
SCAN_ANALYZER:= $(if $(LLVM_BIN),$(LLVM_BIN)/clang++,clang++)
COV_CXX      := $(if $(LLVM_BIN),$(LLVM_BIN)/clang++,clang++)
LLVM_PROFDATA:= $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-profdata,llvm-profdata)
LLVM_COV     := $(if $(LLVM_BIN),$(LLVM_BIN)/llvm-cov,llvm-cov)

# Compiler
CXX       ?= g++

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

# Include path: do NOT hardcode /opt/homebrew/include by default
CXXFLAGS  := -std=c++17 -fno-exceptions -fno-rtti \
             -Wall -Wextra -Wpedantic -Werror \
             -Wshadow -Wconversion -Wsign-conversion \
             -Wcast-align -Wformat=2 -Wnull-dereference \
             -Wdouble-promotion -Wno-unknown-pragmas \
			 -Isrc $(MBEDTLS_CFLAGS) -g

# mbedTLS linking (portable default). Users can override.
# On Ubuntu with libmbedtls-dev installed, these libs are on the default linker path.
MBEDTLS_LIBS ?= $(if $(strip $(MBEDTLS_PKG_LIBS)),$(MBEDTLS_PKG_LIBS),$(if $(strip $(MBEDTLS_FALLBACK_LIBS)),$(MBEDTLS_FALLBACK_LIBS),-lmbedtls -lmbedx509 -lmbedcrypto))
LDFLAGS      := -lpthread $(MBEDTLS_LIBS)

# ─────────────────────────────────────────────────────────────────────────────
# Source groups
# ─────────────────────────────────────────────────────────────────────────────
CORE_SRC := \
    src/core/Serializer.cpp \
    src/core/MessageId.cpp \
    src/core/Timestamp.cpp \
    src/core/DuplicateFilter.cpp \
    src/core/AckTracker.cpp \
    src/core/RetryManager.cpp \
    src/core/DeliveryEngine.cpp \
    src/core/AssertState.cpp

PLATFORM_SRC := \
    src/platform/PrngEngine.cpp \
    src/platform/ImpairmentEngine.cpp \
    src/platform/ImpairmentConfigLoader.cpp \
    src/platform/SocketUtils.cpp \
    src/platform/TcpBackend.cpp \
    src/platform/TlsTcpBackend.cpp \
    src/platform/UdpBackend.cpp \
    src/platform/DtlsUdpBackend.cpp \
    src/platform/LocalSimHarness.cpp \
    src/platform/MbedtlsOpsImpl.cpp \
    src/platform/SocketOpsImpl.cpp

ALL_LIB_SRC := $(CORE_SRC) $(PLATFORM_SRC)

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
.PHONY: all clean tests server client check_traceability \
        lint cppcheck pclint scan_build static_analysis \
        coverage coverage_show coverage_report

all: server client tests

server: $(ALL_LIB_OBJS) build/objs/app/Server.o
	$(CXX) $(CXXFLAGS) -o build/server $^ $(LDFLAGS)

client: $(ALL_LIB_OBJS) build/objs/app/Client.o
	$(CXX) $(CXXFLAGS) -o build/client $^ $(LDFLAGS)

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
    build/test_TlsTcpBackend \
    build/test_DtlsUdpBackend \
    build/test_TcpBackend \
    build/test_UdpBackend \
    build/test_SocketUtils \
    build/test_AssertState

build/test_%: $(ALL_LIB_OBJS) build/objs/tests/test_%.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

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
	@$(CLANG_TIDY) $(shell find src -name '*.cpp') -- $(CXXFLAGS)
	@echo "=== Clang-Tidy: tests/ (relaxed) ==="
	@$(CLANG_TIDY) $(shell find tests -name '*.cpp') -- $(CXXFLAGS)
	@echo "=== Clang-Tidy: PASS ==="

# Tier 2b: Cppcheck flow/style analysis (CI-safe; no addon required).
# MISRA C++:2023 enforcement is covered by clang-tidy (Tier 2a, above).
# Config: .cppcheck-suppress (documented deviation suppressions).
#       On macOS: brew install cppcheck
#       On Linux: apt install cppcheck
cppcheck:
	@echo "=== Cppcheck: src/ ==="
	@grep -v '^#' .cppcheck-suppress | grep -v '^$$' | \
	    cppcheck --enable=all --error-exitcode=1 \
	    --suppressions-list=/dev/stdin \
	    --std=c++17 -I src \
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
SCAN_ANALYZER := $(if $(LLVM_BIN),$(LLVM_BIN)/clang++,clang++)
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
TEST_NAMES    := MessageEnvelope Serializer DuplicateFilter ImpairmentEngine LocalSim AckTracker RetryManager DeliveryEngine ImpairmentConfigLoader TlsTcpBackend DtlsUdpBackend TcpBackend UdpBackend SocketUtils AssertState
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
	@LLVM_PROFILE_FILE="build/cov/TlsTcpBackend.profraw" build/cov_test_TlsTcpBackend >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/DtlsUdpBackend.profraw" build/cov_test_DtlsUdpBackend >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/TcpBackend.profraw"    build/cov_test_TcpBackend    >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/UdpBackend.profraw"    build/cov_test_UdpBackend    >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/SocketUtils.profraw"  build/cov_test_SocketUtils   >/dev/null 2>&1
	@LLVM_PROFILE_FILE="build/cov/AssertState.profraw"  build/cov_test_AssertState   >/dev/null 2>&1
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
	    build/cov/TlsTcpBackend.profraw \
	    build/cov/DtlsUdpBackend.profraw \
	    build/cov/TcpBackend.profraw \
	    build/cov/UdpBackend.profraw \
	    build/cov/SocketUtils.profraw \
	    build/cov/AssertState.profraw \
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
	    -object build/cov_test_TlsTcpBackend \
	    -object build/cov_test_DtlsUdpBackend \
	    -object build/cov_test_TcpBackend \
	    -object build/cov_test_UdpBackend \
	    -object build/cov_test_SocketUtils \
	    -object build/cov_test_AssertState \
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
	    -object build/cov_test_TlsTcpBackend \
	    -object build/cov_test_DtlsUdpBackend \
	    -object build/cov_test_TcpBackend \
	    -object build/cov_test_UdpBackend \
	    -object build/cov_test_SocketUtils \
	    -object build/cov_test_AssertState \
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
	    -object build/cov_test_TlsTcpBackend \
	    -object build/cov_test_DtlsUdpBackend \
	    -object build/cov_test_TcpBackend \
	    -object build/cov_test_UdpBackend \
	    -object build/cov_test_SocketUtils \
	    -object build/cov_test_AssertState \
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
	    -object build/cov_test_TlsTcpBackend \
	    -object build/cov_test_DtlsUdpBackend \
	    -object build/cov_test_TcpBackend \
	    -object build/cov_test_UdpBackend \
	    -object build/cov_test_SocketUtils \
	    -object build/cov_test_AssertState \
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
	@echo "=== test_TlsTcpBackend ==="; build/test_TlsTcpBackend
	@echo "=== test_DtlsUdpBackend ==="; build/test_DtlsUdpBackend
	@echo "=== test_TcpBackend ==="; build/test_TcpBackend
	@echo "=== test_UdpBackend ==="; build/test_UdpBackend
	@echo "=== test_SocketUtils ==="; build/test_SocketUtils
	@echo "=== test_AssertState ==="; build/test_AssertState
	@echo "=== ALL TESTS PASSED ==="
