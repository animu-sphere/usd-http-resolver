# SPDX-License-Identifier: Apache-2.0
#
# The bundle's tests, in two groups that differ in what they need.
#
# The first four link one translation unit out of src/ and nothing else: no
# OpenUSD, no sockets. Identifier normalization, the configuration surface, the
# HTTPxxx projection, and what may be published about an asset's identity are
# arithmetic, and keeping them testable that way is why they are separate
# translation units rather than lambdas inside the resolver.
#
# The fifth is the release's headline claim -- a remote stage, over a socket,
# through the registered plugin -- and it needs a runtime and an origin. It is
# also the one place in this repository that links OpenUSD and the hostile
# fixture corpus at once, which WORKSPACE.md §2 admits by name.

set(_http_resolver_src "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# One executable per translation unit under test, which is also what keeps the
# claim honest: if `test_identifier` linked the whole bundle it would prove
# nothing about whether the normalization can be reasoned about on its own.
function(http_resolver_offline_test _name _source)
    add_executable(httpResolver_test_${_name}
        test_${_name}.cpp
        "${_http_resolver_src}/${_source}.cpp")
    target_include_directories(httpResolver_test_${_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${_http_resolver_src}")
    if(MSVC)
        target_compile_options(httpResolver_test_${_name} PRIVATE /utf-8 /W4)
    else()
        target_compile_options(httpResolver_test_${_name}
                               PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME httpResolver_${_name} COMMAND httpResolver_test_${_name})
endfunction()

http_resolver_offline_test(configuration Configuration)
http_resolver_offline_test(diagnostics Diagnostics)
http_resolver_offline_test(identifier Identifier)
http_resolver_offline_test(identity Identity)

# `Identifier.cpp` needs nothing at all; the other three need the contracts they
# are expressed in. None needs a transport, and none opens a socket.
target_link_libraries(httpResolver_test_configuration
                      PRIVATE usdasset::http usdasset::cache)
target_link_libraries(httpResolver_test_diagnostics PRIVATE usdasset::io)
target_link_libraries(httpResolver_test_identity PRIVATE usdasset::io)

# The end-to-end test needs the fixture corpus, which exists only in a build
# that includes tests/ -- a standalone `ost plugin build` of this bundle has no
# tests/fixture-server. Guarded rather than assumed, so the bundle still
# configures on its own.
if(NOT TARGET usdasset::fixtureserver)
    message(STATUS
            "http-resolver: fixture server absent; skipping the stage test")
    return()
endif()

add_executable(httpResolver_test_stage test_stage.cpp)
target_include_directories(httpResolver_test_stage PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${PXR_INCLUDE_DIRS})
target_link_libraries(httpResolver_test_stage PRIVATE usdasset::fixtureserver)

# The stage test composes a stage, so it needs more of OpenUSD than the bundle
# itself does. That asymmetry is the right way round: the plugin links only Ar,
# and the test that proves a consumer can use it links what a consumer would.
openstrata_link_openusd(
    TARGET httpResolver_test_stage
    COMPONENTS arch tf js plug vt ar sdf usd)

if(MSVC)
    # NOMINMAX is not hygiene here, it is required to compile: OpenUSD's arch
    # headers reach Windows.h, whose `min` and `max` macros then eat the
    # `numeric_limits` specializations in gf's half support. The bundle target
    # gets this from `openstrata_configure_plugin`; a test that includes more of
    # OpenUSD than the bundle does has to say it itself.
    target_compile_definitions(httpResolver_test_stage PRIVATE NOMINMAX)
    target_compile_options(httpResolver_test_stage PRIVATE /utf-8 /W4)
else()
    target_compile_options(httpResolver_test_stage PRIVATE -Wall -Wextra)
endif()

# The bundle must be built before the test can load it: the test finds the
# plugin through its plugInfo.json, and the plugInfo.json points at a library
# that has to exist.
add_dependencies(httpResolver_test_stage HttpResolver)

add_test(NAME httpResolver_stage COMMAND httpResolver_test_stage)

# How OpenUSD finds the bundle. Pointed at the bundle in the source tree rather
# than at an install prefix, so that `ctest` after a build tests what was just
# built and needs no install step.
set_tests_properties(httpResolver_stage PROPERTIES
    ENVIRONMENT
        "PXR_PLUGINPATH_NAME=${CMAKE_CURRENT_SOURCE_DIR}/../plugin/resources/httpResolver")

# On Windows, OpenUSD is reached by putting the runtime on PATH rather than by
# copying its DLLs beside the executable, which is what
# `usd_http_resolver_stage_runtime_dependencies` does for every other test here
# and what must not happen to this one.
#
# The difference is not tidiness. `PlugRegistry` finds OpenUSD's own
# plugInfo.json files *relative to the library that loaded*, so a copied
# usd_sdf.dll comes up with no file formats registered, and the process dies in
# static initialization with no output at all -- which reads exactly like a
# broken resolver and is a broken deployment of USD. The other tests here link
# no shared library at all (libcurl is static, per ADR-0003), so the copy is a
# no-op for them and this is the only target that has to say anything.
if(WIN32)
    set_tests_properties(httpResolver_stage PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${PXR_CMAKE_DIR}/lib;PATH=path_list_prepend:${PXR_CMAKE_DIR}/bin")
endif()
