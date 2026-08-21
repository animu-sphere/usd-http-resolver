# SPDX-License-Identifier: Apache-2.0
#
# The recorded I/O baseline: the five scenarios METRICS.md §6 requires.
#
# This is the fourth legal reverse edge in WORKSPACE.md §2 -- a test outside
# libs/ that links both a backend and the fixture server -- and it is here for
# the same reason the other three are: a module's tests must not depend on
# anything outside libs/, or `ost library build libs/usd-asset-http`, which
# builds the module alone, stops working.
#
# It also compiles the fixture server's own raw client, which nothing outside
# that server's own self-test has needed until now. The reason is the point of
# METRICS.md §6's fourth row: "must not be worse than a plain download" needs a
# plain download performed by something that is not the client under test. The
# raw client shares no HTTP code with `usdAssetHttp`, so it is evidence rather
# than a tautology -- the same separation the boundary suite keeps between its
# oracle and `usdAssetLocal`.

add_executable(usdAssetHttp_baseline
    baseline_main.cpp
    Report.cpp
    ../fixture-server/tests/RawClient.cpp)

# The cache joins the link line in v0.3.0: every scenario is measured with and
# without it, because METRICS.md section 6 asks a release that changes I/O
# behavior for the counter values before and after, and this is the release that
# changes them on purpose.
target_link_libraries(usdAssetHttp_baseline
    PRIVATE usdasset::http usdasset::cache usdasset::fixtureserver)
target_include_directories(usdAssetHttp_baseline PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixture-server/tests"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixture-server/src")

# What a release record has to state beside the numbers, per METRICS.md §6 and
# the record contents in releases/README.md. Compiled in rather than detected at
# run time: a baseline that cannot say which toolchain and configuration
# produced it is a table of numbers, and gate 6 compares tables across releases.
target_compile_definitions(usdAssetHttp_baseline PRIVATE
    USD_ASSET_BASELINE_CONFIG="$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,unspecified>"
    USD_ASSET_BASELINE_TOOLCHAIN="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    USD_ASSET_BASELINE_PLATFORM="${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")

if(MSVC)
    target_compile_options(usdAssetHttp_baseline PRIVATE /utf-8 /W4)
    # `getenv` and `fopen`, as everywhere else in this tree's tests: the _s
    # variants exist on one toolchain, and a harness that is a different program
    # on one platform is a harness whose numbers are about the platform.
    target_compile_definitions(usdAssetHttp_baseline PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(usdAssetHttp_baseline PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(COMMAND usd_http_resolver_stage_runtime_dependencies)
    usd_http_resolver_stage_runtime_dependencies(usdAssetHttp_baseline)
endif()

add_test(NAME usdAssetHttp_io_baseline COMMAND usdAssetHttp_baseline)

# Registered as a test and not only as a tool a release run invokes, because
# gate 6 is a regression gate: the byte counts here are exact, and a cache, a
# retry, or a redirect that starts happening where none did is a lane failure
# rather than a number somebody notices at release time. The measurement itself
# is what a release records, and it is reproducible by hand:
#
#     ./build/core/tests/baseline/usdAssetHttp_baseline --output baseline.md
#
set_tests_properties(usdAssetHttp_io_baseline PROPERTIES TIMEOUT 600)

# A sanitizer lane measures nothing -- 128 MiB of instrumented memcpy over
# loopback is a lane that times out -- but the counter assertions are worth
# running under one, because they are the assertions a data race in a parallel
# reader would break. So the lane keeps the scenarios and shrinks the fixture,
# and the report it prints says which size it used, so no run of it can be
# mistaken for the recorded baseline.
if(USD_HTTP_RESOLVER_SANITIZER)
    set_tests_properties(usdAssetHttp_io_baseline PROPERTIES
        ENVIRONMENT "USD_ASSET_BASELINE_ASSET_BYTES=8388608")
endif()
