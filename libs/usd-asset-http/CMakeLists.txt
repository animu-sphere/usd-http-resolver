# SPDX-License-Identifier: Apache-2.0
#
# usdAssetHttp -- the HTTP backend, and the one module in this repository that
# is permitted to name an HTTP client (WORKSPACE.md §2, invariant 4).
#
# The dependency is private in both senses CMake distinguishes: `CURL::libcurl`
# is linked PRIVATE, so no consumer of this static library inherits it, and no
# installed header includes `curl.h`. That is ADR-0003's recorded consequence,
# and it is what keeps the ADR cheap to supersede.

cmake_minimum_required(VERSION 3.23)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" _usd_asset_http_version
         LIMIT_COUNT 1)
    string(STRIP "${_usd_asset_http_version}" _usd_asset_http_version)
else()
    set(_usd_asset_http_version "0.2.0")
endif()

project(usdAssetHttp
    VERSION ${_usd_asset_http_version}
    DESCRIPTION "HTTP range-request random-access backend"
    LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED USDASSETHTTP_BUILD_TESTS)
    if(DEFINED USD_HTTP_RESOLVER_BUILD_TESTS)
        set(_usd_asset_http_tests_default "${USD_HTTP_RESOLVER_BUILD_TESTS}")
    else()
        set(_usd_asset_http_tests_default "${PROJECT_IS_TOP_LEVEL}")
    endif()
    option(USDASSETHTTP_BUILD_TESTS "Build usdAssetHttp tests"
           ${_usd_asset_http_tests_default})
endif()

# Standalone builds (`ost library build libs/usd-asset-http`) resolve the
# dependency; in-tree builds already have the target from the root's ordered
# library list.
if(NOT TARGET usdasset::io)
    find_package(usdAssetIo CONFIG REQUIRED)
endif()

# The one third-party dependency in this repository, chosen on stated criteria
# in ADR-0003 and acquired the way that ADR names: a `find_package`, with CI
# installing per platform, rather than vendored source or a pinned FetchContent
# that slows every cold build.
#
#   windows   vcpkg install curl
#   linux     apt-get install libcurl4-openssl-dev
#   macos     brew install curl
#
# REQUIRED, so that its absence is a configure-time failure with a name on it
# rather than a link error three minutes into a build.
find_package(CURL REQUIRED)

add_library(usdAssetHttp STATIC
    src/CurlTransport.cpp
    src/Framing.cpp
    src/HttpAssetReader.cpp
    src/Transport.cpp
    src/Uri.cpp)
add_library(usdasset::http ALIAS usdAssetHttp)
set_target_properties(usdAssetHttp PROPERTIES
    EXPORT_NAME http
    POSITION_INDEPENDENT_CODE ON)

target_compile_features(usdAssetHttp PUBLIC cxx_std_17)
target_include_directories(usdAssetHttp
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(usdAssetHttp
    PUBLIC usdasset::io
    PRIVATE CURL::libcurl)

if(MSVC)
    target_compile_options(usdAssetHttp PRIVATE /utf-8 /W4)
else()
    target_compile_options(usdAssetHttp PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS usdAssetHttp
    EXPORT usdAssetHttpTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/usdAssetHttpConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetHttpConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetHttp")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetHttpConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT usdAssetHttpTargets
    FILE usdAssetHttpTargets.cmake
    NAMESPACE usdasset::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetHttp")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetHttpConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetHttpConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetHttp")

if(USDASSETHTTP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
