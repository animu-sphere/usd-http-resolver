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
    if(COMMAND usd_http_resolver_stage_runtime_dependencies)
        usd_http_resolver_stage_runtime_dependencies(${_target})
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

# The cache's row: the same local backend with a decorator over it, which is
# what makes "byte-for-byte equivalence with the uncached path over the full
# suite" an assertion rather than a claim. Not a fourth backend -- the cache is
# not a transport -- and it links no transport, which is the property that keeps
# the local backend a usable oracle for the cached path.
if(TARGET usdasset::cache AND TARGET usdasset::local)
    usd_http_resolver_add_boundary_backend(cached_local
        backends/boundary_cached_local_main.cpp usdasset::cache usdasset::local)
endif()

# The line v0.2.0 said it would be. The HTTP row additionally links the fixture
# server, which is the one reverse edge WORKSPACE.md §2 admits for it: a remote
# backend has to arrange for its bytes to exist somewhere a transport can reach,
# and that is exactly what fixture provisioning means for it.
if(TARGET usdasset::http)
    usd_http_resolver_add_boundary_backend(http
        backends/boundary_http_main.cpp usdasset::http usdasset::fixtureserver)
endif()
