# ─────────────────────────────────────────────────────────────────────────────
# Makefile for messageEngine
# Enforces Power of 10 zero-warnings policy (rule 10).
# No cmake required; pure GNU make.
# ─────────────────────────────────────────────────────────────────────────────

CXX       := g++
CXXFLAGS  := -std=c++17 -fno-exceptions -fno-rtti \
             -Wall -Wextra -Wpedantic -Werror \
             -Wshadow -Wconversion -Wsign-conversion \
             -Wcast-align -Wformat=2 -Wnull-dereference \
             -Wdouble-promotion -Wno-unknown-pragmas \
             -Isrc -I/opt/homebrew/include -g

LDFLAGS   := -lpthread -L/opt/homebrew/lib -lmbedtls -lmbedx509 -lmbedcrypto

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
    src/core/DeliveryEngine.cpp

PLATFORM_SRC := \
    src/platform/PrngEngine.cpp \
    src/platform/ImpairmentEngine.cpp \
    src/platform/ImpairmentConfigLoader.cpp \
    src/platform/SocketUtils.cpp \
    src/platform/TcpBackend.cpp \
    src/platform/TlsTcpBackend.cpp \
    src/platform/UdpBackend.cpp \
    src/platform/LocalSimHarness.cpp

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
    build/test_TlsTcpBackend

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
	@PATH="/opt/homebrew/opt/llvm/bin:$$PATH" clang-tidy $(shell find src -name '*.cpp') -- $(CXXFLAGS)
	@echo "=== Clang-Tidy: tests/ (relaxed) ==="
	@PATH="/opt/homebrew/opt/llvm/bin:$$PATH" clang-tidy $(shell find tests -name '*.cpp') -- $(CXXFLAGS)
	@echo "=== Clang-Tidy: PASS ==="

# Tier 2b: Cppcheck with MISRA C++:2023 addon
# Config: .cppcheck-suppress (documented deviation suppressions).
# Note: the MISRA addon (misra.py) must be installed with Cppcheck.
#       On macOS: brew install cppcheck
#       On Linux: apt install cppcheck python3-cppcheck-addons (or build from source)
cppcheck:
	@echo "=== Cppcheck + MISRA C++:2023 addon: src/ ==="
	@cppcheck --enable=all --error-exitcode=1 \
	    --addon=misra \
	    --suppressions-list=.cppcheck-suppress \
	    --std=c++17 -I src \
	    src/ 2>&1
	@echo "=== Cppcheck: PASS ==="

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
SCAN_BUILD    := /opt/homebrew/opt/llvm/bin/scan-build
SCAN_ANALYZER := /opt/homebrew/opt/llvm/bin/clang++
CXX_ANALYZER  := /opt/homebrew/opt/llvm/libexec/c++-analyzer
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
	@$(SCAN_BUILD) \
	    --use-analyzer=$(SCAN_ANALYZER) \
	    --status-bugs \
	    -o build/scan-build \
	    -enable-checker alpha.core.BoolAssignment \
	    -enable-checker alpha.core.Conversion \
	    -enable-checker alpha.unix.cstring.OutOfBounds \
	    $(MAKE) $(SCAN_OBJS) \
	        CXX="$(CXX_ANALYZER)" \
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
COV_CXX       := /opt/homebrew/opt/llvm/bin/clang++
LLVM_PROFDATA := /opt/homebrew/opt/llvm/bin/llvm-profdata
LLVM_COV      := /opt/homebrew/opt/llvm/bin/llvm-cov
COV_OBJ_DIR   := build/cov-objs
# Strip -Werror so clang++ dialect differences do not block coverage builds;
# add instrumentation flags; disable optimisation for accurate line mapping.
COV_CXXFLAGS  := $(filter-out -Werror,$(CXXFLAGS)) \
                 -fprofile-instr-generate -fcoverage-mapping -O0
COV_LDFLAGS   := -fprofile-instr-generate $(LDFLAGS)
COV_LIB_OBJS  := $(patsubst src/%.cpp,$(COV_OBJ_DIR)/%.o,$(ALL_LIB_SRC))
TEST_NAMES    := MessageEnvelope Serializer DuplicateFilter ImpairmentEngine LocalSim AckTracker RetryManager DeliveryEngine ImpairmentConfigLoader TlsTcpBackend
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
	@echo "  Known gaps (SC files with no test suite -- blocking defects):"
	@echo "    platform/TcpBackend.cpp          0%  no test suite"
	@echo "    platform/UdpBackend.cpp          0%  no test suite"
	@echo ""
	@echo "  NSC files (line coverage sufficient, branch floor not enforced):"
	@echo "    platform/LocalSimHarness.cpp"
	@echo "    platform/SocketUtils.cpp   (UDP socket helpers untested)"
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
	@echo "=== ALL TESTS PASSED ==="
