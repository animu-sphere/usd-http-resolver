# SPDX-License-Identifier: Apache-2.0
#
# http-resolver -- the `ArResolver` bundle for `http` and `https`.
#
# The only directory in this repository that resolves OpenUSD. The root adds it
# under `USD_HTTP_RESOLVER_BUILD_PLUGIN`, and never otherwise, which is what
# keeps `libs/` buildable and testable on a machine with no OpenUSD installed
# (WORKSPACE.md §2.1).
#
# Self-contained, so that `ost plugin build plugins/http-resolver` can configure
# it directly, and `add_subdirectory()`-able from the root for the plain-CMake
# workflow. The two differ only in how the dependencies arrive: in-tree they are
# already targets, standalone they are packages.
cmake_minimum_required(VERSION 3.23)

# The repo-root VERSION is the single source of the release version. A bundle
# exported without the root falls back to the manifest's, so that a standalone
# configure still works rather than failing on a missing file.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" _http_resolver_version
         LIMIT_COUNT 1)
    string(STRIP "${_http_resolver_version}" _http_resolver_version)
else()
    set(_http_resolver_version "0.5.0")  # mirrors openstrata.plugin.yaml
endif()

project(HttpResolverPlugin
    VERSION ${_http_resolver_version}
    DESCRIPTION "OpenUSD ArResolver for http and https range reads"
    LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/OpenStrataPlugin.cmake")
openstrata_default_build_type()

if(NOT DEFINED HTTP_RESOLVER_BUILD_TESTS)
    if(DEFINED USD_HTTP_RESOLVER_BUILD_TESTS)
        set(_http_resolver_tests_default "${USD_HTTP_RESOLVER_BUILD_TESTS}")
    else()
        set(_http_resolver_tests_default "${PROJECT_IS_TOP_LEVEL}")
    endif()
    option(HTTP_RESOLVER_BUILD_TESTS "Build the http-resolver bundle tests"
           ${_http_resolver_tests_default})
endif()

# Resolved from the runtime `ost` activates, or from CMAKE_PREFIX_PATH in the
# plain-CMake flow. Idempotent when the root has already found it.
if(NOT pxr_FOUND)
    find_package(pxr CONFIG REQUIRED)
endif()

# The backend, and through it the contracts and the counters. In-tree this is
# already a target; standalone it is a package `ost` has installed into the
# workspace prefix.
if(NOT TARGET usdasset::http)
    find_package(usdAssetHttp CONFIG REQUIRED)
endif()

# And the cache, resolved the same way. It is not a backend and knows no
# transport: it decorates the reader the backend returns.
if(NOT TARGET usdasset::cache)
    find_package(usdAssetCache CONFIG REQUIRED)
endif()

set(PLUGIN_NAME HttpResolver)

add_library(${PLUGIN_NAME} SHARED
    src/Configuration.cpp
    src/Diagnostics.cpp
    src/HttpResolver.cpp
    src/Identifier.cpp
    src/Identity.cpp
    src/Report.cpp
    src/ResolvedAsset.cpp)

target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_17)
target_include_directories(${PLUGIN_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${PXR_INCLUDE_DIRS})
# The cache joins the link line in v0.3.0. WORKSPACE.md section 2 has admitted
# the edge since the workspace contract was written; this is the release in
# which the bundle takes it.
target_link_libraries(${PLUGIN_NAME} PRIVATE usdasset::http usdasset::cache)

if(MSVC)
    target_compile_options(${PLUGIN_NAME} PRIVATE /utf-8 /W4)
else()
    target_compile_options(${PLUGIN_NAME} PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Only the Ar surface of OpenUSD. A resolver reads bytes and hands them over; it
# does not author stages, so no usd/usdGeom component appears here. The narrower
# the link, the smaller the claim this bundle makes on a host's runtime.
openstrata_link_openusd(
    TARGET ${PLUGIN_NAME}
    COMPONENTS arch tf js plug vt ar)

# Stages the shared library into the bundle's lib/, so that the committed
# plugInfo.json's LibraryPath resolves, and regenerates that plugInfo.json for
# the target platform's library naming.
openstrata_configure_plugin(
    TARGET ${PLUGIN_NAME}
    PLUG_INFO_INPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/resources/httpResolver/plugInfo.json.in"
    PLUG_INFO_OUTPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/resources/httpResolver/plugInfo.json")

openstrata_install_plugin_bundle(
    TARGET ${PLUGIN_NAME}
    RESOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin/resources/httpResolver/plugInfo.json"
    RESOURCE_DESTINATION "plugin/resources/httpResolver"
    MANIFESTS
        "${CMAKE_CURRENT_SOURCE_DIR}/openstrata.plugin.yaml"
    FIXTURE_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")

if(HTTP_RESOLVER_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
