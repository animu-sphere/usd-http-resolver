# SPDX-License-Identifier: Apache-2.0
#
# The shared boundary suite: a library of cases, plus one small executable per
# backend that is admitted by it.
#
# The suite links usdasset::io and nothing else. It must not acquire a
# dependency on any backend, because the moment it can name one it can special
# case one, and a per-backend exception is how a contract stops being one.

add_library(usdAssetBoundary STATIC
    src/Fixture.cpp
    src/Oracle.cpp
    src/Random.cpp
    src/Report.cpp
    src/Suite.cpp)
add_library(usdasset::boundary ALIAS usdAssetBoundary)

target_compile_features(usdAssetBoundary PUBLIC cxx_std_17)
target_include_directories(usdAssetBoundary
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(usdAssetBoundary PUBLIC usdasset::io)

if(MSVC)
    target_compile_options(usdAssetBoundary PRIVATE /utf-8 /W4)
    # The oracle is fopen/fseek/fread on purpose: it is a second, naive
    # implementation, and swapping it for the MSVC-only _s variants would make
    # it a different program on one platform.
    target_compile_definitions(usdAssetBoundary PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(usdAssetBoundary PRIVATE -Wall -Wextra -Wpedantic)
endif()

# --- rows --------------------------------------------------------------------
#
# One executable per backend. Each registers three CTest entries rather than
# one, so a failure names which part of the contract broke instead of turning
# the whole suite one opaque red.
function(usd_http_resolver_add_boundary_backend _name _source)
    set(_target boundary_${_name})
    add_executable(${_target} "${_source}")
    target_link_libraries(${_target} PRIVATE usdasset::boundary ${ARGN})
    if(MSVC)
        target_compile_options(${_target} PRIVATE /utf-8)
    endif()
    foreach(_group fixed property concurrent)
        add_test(NAME boundary_${_name}_${_group}
                 COMMAND ${_target} ${_group})
    endforeach()
endfunction()

if(TARGET usdasset::local)
    usd_http_resolver_add_boundary_backend(local
        backends/boundary_local_main.cpp usdasset::local)
endif()

# v0.2.0 adds one line here:
#
#   usd_http_resolver_add_boundary_backend(http
#       backends/boundary_http_main.cpp usdasset::http)
