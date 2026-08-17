# SPDX-License-Identifier: Apache-2.0
#
# The corpus projection: which hostile server behavior produces which
# StatusCode.
#
# This is the second of the two legal reverse edges in WORKSPACE.md §2 -- a test
# outside libs/ that links both a backend and the fixture server. It lives here
# rather than under libs/usd-asset-http/tests because a module's tests must not
# depend on anything outside libs/: `ost library build libs/usd-asset-http`
# builds the module alone, and a test that needed tests/fixture-server would
# make that impossible.
#
# The direction stays one-way. The fixture server links neither this target nor
# anything it names, and it still does not know what a StatusCode is.

add_executable(usdAssetHttp_test_projection test_projection.cpp)

target_link_libraries(usdAssetHttp_test_projection
    PRIVATE usdasset::http usdasset::fixtureserver)
target_include_directories(usdAssetHttp_test_projection
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(MSVC)
    target_compile_options(usdAssetHttp_test_projection PRIVATE /utf-8 /W4)
else()
    target_compile_options(usdAssetHttp_test_projection PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(COMMAND usd_http_resolver_stage_runtime_dependencies)
    usd_http_resolver_stage_runtime_dependencies(usdAssetHttp_test_projection)
endif()

add_test(NAME usdAssetHttp_corpus_projection COMMAND usdAssetHttp_test_projection)

# Two deliberate stalls of well under a second each, plus a redirect chain and a
# retry. Naming a timeout is not about the expected duration: it is so that a
# *hung* exchange fails the lane rather than holding it, which is the failure
# mode a suite full of deadlines has.
set_tests_properties(usdAssetHttp_corpus_projection PROPERTIES TIMEOUT 120)
