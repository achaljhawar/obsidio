cmake_minimum_required(VERSION 3.22)
project(obsidio_cpp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# NEVER -march=native. The grader builds this image on its own machine, so a
# native build is both non-reproducible and a possible illegal-instruction
# crash. Portable codegen plus runtime CPU dispatch inside the hash backend is
# the correct answer.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -fno-plt")

option(OBSIDIO_SANITIZE "Build with ASan + UBSan (debug builds only)" OFF)
option(OBSIDIO_OPENSSL "Use OpenSSL's SHA-256 (hardware accelerated)" ON)

set(OBSIDIO_SOURCES
  src/sha256.cpp
  src/risk.cpp
  src/chain_arm.cpp
  src/chain_x86.cpp
  src/data.cpp
  src/http_server.cpp
  src/persist.cpp
)

add_library(obsidio_core STATIC ${OBSIDIO_SOURCES})
target_include_directories(obsidio_core PUBLIC src)
target_compile_options(obsidio_core PRIVATE -Wall -Wextra)

# The accelerated back ends are the ONLY translation units allowed to contain
# their SIMD crypto instructions, so each flag goes on its file alone rather
# than on the target. Everything else stays baseline-portable, and nothing in
# either file is reached until the CPU has confirmed the feature at runtime --
# so the same binary starts on an ARM core without HWCAP_SHA2 or an x86 core
# without CPUID sha. This is what lets us use the extensions without
# -march=native.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set_source_files_properties(src/chain_arm.cpp PROPERTIES
    COMPILE_OPTIONS "-march=armv8-a+crypto")
  message(STATUS "risk chain: ARMv8 crypto back end compiled in (runtime-gated)")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
  # Same rule as the ARM back end: SHA-NI instructions are confined to one
  # translation unit, entered only after __builtin_cpu_supports("sha") says
  # yes, so the binary still starts on any x86-64 CPU.
  set_source_files_properties(src/chain_x86.cpp PROPERTIES
    COMPILE_OPTIONS "-msha;-mssse3;-msse4.1")
  message(STATUS "risk chain: x86 SHA-NI back end compiled in (runtime-gated)")
else()
  message(STATUS "risk chain: no accelerated back end for ${CMAKE_SYSTEM_PROCESSOR}"
                 " -- falling back to per-call SHA-256")
endif()

if(OBSIDIO_OPENSSL)
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(obsidio_core PUBLIC OBSIDIO_USE_OPENSSL)
  target_link_libraries(obsidio_core PUBLIC OpenSSL::Crypto)
  message(STATUS "SHA-256 backend: OpenSSL (hardware accelerated)")
else()
  message(STATUS "SHA-256 backend: portable scalar (SLOW -- baseline only)")
endif()

find_package(Threads REQUIRED)
target_link_libraries(obsidio_core PUBLIC Threads::Threads)

if(OBSIDIO_SANITIZE)
  # Run the k6 script against a sanitised build at least once. ASan/UBSan turn
  # silent memory corruption into a loud stack trace, which is the whole reason
  # C++ is survivable here. Never ship this build -- it is ~2x slower.
  target_compile_options(obsidio_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(obsidio_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(obsidio-server src/main.cpp src/risk_pool.cpp)
target_link_libraries(obsidio-server PRIVATE obsidio_core)

add_executable(obsidio-selftest tests/selftest.cpp)
target_link_libraries(obsidio-selftest PRIVATE obsidio_core)

# Socket-level HTTP tests. These drive a real obsidio-server process over a
# real socket, so they need the server binary's path -- not a link against it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(obsidio-http-test tests/http_test.cpp)
  target_compile_options(obsidio-http-test PRIVATE -Wall -Wextra)
  add_dependencies(obsidio-http-test obsidio-server)
endif()

enable_testing()
add_test(NAME selftest COMMAND obsidio-selftest)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME http_input
           COMMAND obsidio-http-test $<TARGET_FILE:obsidio-server>)
  # The suite binds a fixed port, so it must not race another copy of itself.
  set_tests_properties(http_input PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
endif()
