cmake_minimum_required(VERSION 3.16)
project(messageEngine CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Zero-warnings policy (Power of 10 rule 10)
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-align
    -Wformat=2
    -Wnull-dereference
    -Wdouble-promotion
    -Wno-unknown-pragmas
)

# Disable exceptions and RTTI (F-Prime style subset)
add_compile_options(-fno-exceptions -fno-rtti)

include_directories(src)

# ─────────────────────────────────────────────
# Core library (message layer)
# ─────────────────────────────────────────────
add_library(core STATIC
    src/core/Serializer.cpp
    src/core/MessageId.cpp
    src/core/Timestamp.cpp
    src/core/DuplicateFilter.cpp
    src/core/AckTracker.cpp
    src/core/RetryManager.cpp
    src/core/DeliveryEngine.cpp
)
target_include_directories(core PUBLIC src)

# ─────────────────────────────────────────────
# Platform library (backends + impairment)
# ─────────────────────────────────────────────
add_library(platform STATIC
    src/platform/PrngEngine.cpp
    src/platform/ImpairmentEngine.cpp
    src/platform/SocketUtils.cpp
    src/platform/TcpBackend.cpp
    src/platform/UdpBackend.cpp
    src/platform/LocalSimHarness.cpp
)
target_include_directories(platform PUBLIC src)
target_link_libraries(platform PUBLIC core pthread)

# ─────────────────────────────────────────────
# Server application
# ─────────────────────────────────────────────
add_executable(server src/app/Server.cpp)
target_link_libraries(server PRIVATE platform core pthread)

# ─────────────────────────────────────────────
# Client application
# ─────────────────────────────────────────────
add_executable(client src/app/Client.cpp)
target_link_libraries(client PRIVATE platform core pthread)

# ─────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────
enable_testing()

foreach(TEST_NAME
        test_MessageEnvelope
        test_Serializer
        test_DuplicateFilter
        test_ImpairmentEngine
        test_LocalSim)
    add_executable(${TEST_NAME} tests/${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE platform core pthread)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
