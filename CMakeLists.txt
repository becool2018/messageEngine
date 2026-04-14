cmake_minimum_required(VERSION 3.16)

# ─────────────────────────────────────────────────────────────────────────────
# Project declaration — version extracted from src/core/Version.hpp at
# configure time so CMake and the Makefile always agree.
# ─────────────────────────────────────────────────────────────────────────────
file(STRINGS "src/core/Version.hpp" _ver_line
     REGEX "ME_VERSION_STRING\\[\\]")
string(REGEX REPLACE ".*\"([0-9]+\\.[0-9]+\\.[0-9]+)\".*" "\\1"
       _me_version "${_ver_line}")

project(messageEngine
    VERSION     ${_me_version}
    DESCRIPTION "Safety-critical C++ network messaging library"
    LANGUAGES   CXX
)

# ─────────────────────────────────────────────────────────────────────────────
# Language standard — must match Makefile: C++17, no exceptions, no RTTI.
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Compiler flags — mirrors the Makefile's CXXFLAGS exactly (Power of 10 zero-
# warnings policy, MISRA C++:2023, F-Prime style subset).
# ─────────────────────────────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic -Werror
    -Wshadow -Wconversion -Wsign-conversion
    -Wcast-align -Wformat=2 -Wnull-dereference
    -Wdouble-promotion -Wno-unknown-pragmas
    -fno-exceptions -fno-rtti
    -fstack-protector-strong
)

# ─────────────────────────────────────────────────────────────────────────────
# Release profile — mirrors RELEASE=1 in the Makefile.
# cmake -DCMAKE_BUILD_TYPE=Release enables -O2 -D_FORTIFY_SOURCE=2.
# ─────────────────────────────────────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
    add_compile_definitions(_FORTIFY_SOURCE=2)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# mbedTLS dependency — required for TLS/DTLS transport backends.
# Consumers must have libmbedtls-dev (Linux) or `brew install mbedtls` (macOS).
# ─────────────────────────────────────────────────────────────────────────────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MBEDTLS IMPORTED_TARGET mbedtls mbedx509 mbedcrypto)
endif()

if(NOT MBEDTLS_FOUND)
    # Homebrew fallback for macOS when pkg-config data is absent.
    find_path(MBEDTLS_INCLUDE_DIRS mbedtls/ssl.h
              HINTS /opt/homebrew/opt/mbedtls/include
                    /usr/local/opt/mbedtls/include)
    find_library(MBEDTLS_LIB     mbedtls
                 HINTS /opt/homebrew/opt/mbedtls/lib
                       /usr/local/opt/mbedtls/lib)
    find_library(MBEDX509_LIB    mbedx509
                 HINTS /opt/homebrew/opt/mbedtls/lib
                       /usr/local/opt/mbedtls/lib)
    find_library(MBEDCRYPTO_LIB  mbedcrypto
                 HINTS /opt/homebrew/opt/mbedtls/lib
                       /usr/local/opt/mbedtls/lib)
    if(MBEDTLS_INCLUDE_DIRS AND MBEDTLS_LIB AND MBEDX509_LIB AND MBEDCRYPTO_LIB)
        add_library(mbedtls_combined INTERFACE)
        target_include_directories(mbedtls_combined
            INTERFACE ${MBEDTLS_INCLUDE_DIRS})
        target_link_libraries(mbedtls_combined
            INTERFACE ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
        set(MBEDTLS_TARGET mbedtls_combined)
    else()
        message(FATAL_ERROR
            "mbedTLS not found. Install libmbedtls-dev (Linux) "
            "or brew install mbedtls (macOS).")
    endif()
else()
    set(MBEDTLS_TARGET PkgConfig::MBEDTLS)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Threads — must be found before any target that links Threads::Threads.
# ─────────────────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ─────────────────────────────────────────────────────────────────────────────
# Source lists — mirrors CORE_SRC and PLATFORM_SRC in the Makefile exactly.
# ─────────────────────────────────────────────────────────────────────────────
set(CORE_SRC
    src/core/Serializer.cpp
    src/core/MessageId.cpp
    src/core/Timestamp.cpp
    src/core/DuplicateFilter.cpp
    src/core/AckTracker.cpp
    src/core/RetryManager.cpp
    src/core/DeliveryEngine.cpp
    src/core/RequestReplyEngine.cpp
    src/core/AssertState.cpp
    src/core/Fragmentation.cpp
    src/core/ReassemblyBuffer.cpp
    src/core/OrderingBuffer.cpp
)

set(PLATFORM_SRC
    src/platform/PrngEngine.cpp
    src/platform/ImpairmentEngine.cpp
    src/platform/ImpairmentConfigLoader.cpp
    src/platform/SocketUtils.cpp
    src/platform/TcpBackend.cpp
    src/platform/TlsTcpBackend.cpp
    src/platform/TlsSessionStore.cpp
    src/platform/UdpBackend.cpp
    src/platform/DtlsUdpBackend.cpp
    src/platform/LocalSimHarness.cpp
    src/platform/MbedtlsOpsImpl.cpp
    src/platform/SocketOpsImpl.cpp
    src/platform/PosixSyscallsImpl.cpp
)

set(ALL_LIB_SRC ${CORE_SRC} ${PLATFORM_SRC})

# ─────────────────────────────────────────────────────────────────────────────
# Library targets.
# BUILD_SHARED_LIBS=ON selects the shared library as the default target;
# the default (OFF) produces the static library.
# Both targets are always built; the alias chooses the default.
# ─────────────────────────────────────────────────────────────────────────────
add_library(messageengine_static STATIC ${ALL_LIB_SRC})
target_include_directories(messageengine_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(messageengine_static PUBLIC ${MBEDTLS_TARGET} Threads::Threads)
target_compile_definitions(messageengine_static
    PUBLIC MESSAGEENGINE_STATIC_DEFINE)

add_library(messageengine_shared SHARED ${ALL_LIB_SRC})
target_include_directories(messageengine_shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(messageengine_shared PUBLIC ${MBEDTLS_TARGET} Threads::Threads)
target_compile_definitions(messageengine_shared
    PRIVATE MESSAGEENGINE_BUILD_SHARED)

# Set shared library soname/version.
set_target_properties(messageengine_shared PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Namespace aliases.
add_library(MessageEngine::static ALIAS messageengine_static)
add_library(MessageEngine::shared ALIAS messageengine_shared)

if(BUILD_SHARED_LIBS)
    add_library(MessageEngine::messageengine ALIAS messageengine_shared)
else()
    add_library(MessageEngine::messageengine ALIAS messageengine_static)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Application executables.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(me-server src/app/Server.cpp)
target_link_libraries(me-server PRIVATE MessageEngine::static)

add_executable(me-client src/app/Client.cpp)
target_link_libraries(me-client PRIVATE MessageEngine::static)

add_executable(me-tls-demo src/app/TlsTcpDemo.cpp)
target_link_libraries(me-tls-demo PRIVATE MessageEngine::static)

add_executable(me-dtls-demo src/app/DtlsUdpDemo.cpp)
target_link_libraries(me-dtls-demo PRIVATE MessageEngine::static)

# ─────────────────────────────────────────────────────────────────────────────
# Tests.
# ─────────────────────────────────────────────────────────────────────────────
enable_testing()

foreach(TEST_NAME
        test_MessageEnvelope
        test_Serializer
        test_DuplicateFilter
        test_ImpairmentEngine
        test_LocalSim
        test_AckTracker
        test_RetryManager
        test_DeliveryEngine
        test_ImpairmentConfigLoader
        test_TlsTcpBackend
        test_DtlsUdpBackend
        test_TcpBackend
        test_UdpBackend
        test_SocketUtils
        test_AssertState
        test_MessageId
        test_Timestamp
        test_RequestReplyEngine
        test_Fragmentation
        test_ReassemblyBuffer
        test_OrderingBuffer
        test_PrngEngine
        test_RingBuffer
)
    add_executable(${TEST_NAME} tests/${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE MessageEngine::static)
    target_include_directories(${TEST_NAME}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# ─────────────────────────────────────────────────────────────────────────────
# Stress tests — mirrors STRESS_TEST_BINS in the Makefile.
# Built with identical flags to unit tests; labelled "stress" so they can be
# run selectively:  ctest -L stress
# Excluded from the default ctest run: ctest -LE stress
# ─────────────────────────────────────────────────────────────────────────────
foreach(STRESS_NAME
        test_stress_capacity
        test_stress_e2e
        test_stress_ordering
        test_stress_reconnect
        test_stress_ringbuffer
        test_stress_tcp_fanin
)
    add_executable(${STRESS_NAME} tests/${STRESS_NAME}.cpp)
    target_link_libraries(${STRESS_NAME} PRIVATE MessageEngine::static)
    target_include_directories(${STRESS_NAME}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_test(NAME ${STRESS_NAME} COMMAND ${STRESS_NAME})
    set_tests_properties(${STRESS_NAME} PROPERTIES LABELS "stress")
endforeach()

# ─────────────────────────────────────────────────────────────────────────────
# Install rules.
# ─────────────────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Public headers — install flat into include/messageengine/.
set(PUBLIC_HEADERS_CORE
    src/core/messageengine_export.h
    src/core/MessageEnvelope.hpp
    src/core/TransportInterface.hpp
    src/core/DeliveryEngine.hpp
    src/core/Serializer.hpp
    src/core/Types.hpp
    src/core/ChannelConfig.hpp
    src/core/ImpairmentConfig.hpp
    src/core/TlsConfig.hpp
    src/core/Logger.hpp
    src/core/ProtocolVersion.hpp
    src/core/Version.hpp
    src/core/Assert.hpp
    src/core/AssertState.hpp
    src/core/IResetHandler.hpp
    src/core/AbortResetHandler.hpp
    src/core/DeliveryEvent.hpp
    src/core/DeliveryEventRing.hpp
    src/core/DeliveryStats.hpp
    src/core/RingBuffer.hpp
    src/core/MessageId.hpp
    src/core/Timestamp.hpp
    src/core/RequestReplyEngine.hpp
    src/core/RequestReplyHeader.hpp
)
set(PUBLIC_HEADERS_PLATFORM
    src/platform/TcpBackend.hpp
    src/platform/TlsTcpBackend.hpp
    src/platform/TlsSessionStore.hpp
    src/platform/DtlsUdpBackend.hpp
    src/platform/LocalSimHarness.hpp
    src/platform/ImpairmentEngine.hpp
    src/platform/ImpairmentConfigLoader.hpp
    src/platform/PrngEngine.hpp
)
set(TESTING_HEADERS
    src/platform/ISocketOps.hpp
    src/platform/IPosixSyscalls.hpp
    src/platform/IMbedtlsOps.hpp
)

install(FILES ${PUBLIC_HEADERS_CORE} ${PUBLIC_HEADERS_PLATFORM}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/messageengine)
install(FILES ${TESTING_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/messageengine/testing)

# Libraries.
install(TARGETS messageengine_static messageengine_shared
        EXPORT  MessageEngineTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Executables.
install(TARGETS me-server me-client
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# pkg-config.
configure_file(packaging/messageengine.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/messageengine.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/messageengine.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# CMake package config.
install(EXPORT MessageEngineTargets
        FILE      MessageEngineTargets.cmake
        NAMESPACE MessageEngine::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MessageEngine
)

configure_package_config_file(
    packaging/cmake/MessageEngineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MessageEngineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MessageEngine
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/MessageEngineConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MessageEngineConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MessageEngineConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MessageEngine
)
