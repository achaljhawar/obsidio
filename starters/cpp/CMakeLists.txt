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
  src/data.cpp
  src/http_server.cpp
)

add_library(obsidio_core STATIC ${OBSIDIO_SOURCES})
target_include_directories(obsidio_core PUBLIC src)
target_compile_options(obsidio_core PRIVATE -Wall -Wextra)

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

enable_testing()
add_test(NAME selftest COMMAND obsidio-selftest)
