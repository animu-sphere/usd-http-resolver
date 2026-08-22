# SPDX-License-Identifier: Apache-2.0
#
# usdAssetCache -- the block cache, as a decorator over AssetReader.
#
# It links usdasset::io and nothing else. Not a backend, not a transport, not
# OpenUSD: the cache holds a reader it did not construct, and if it knew what
# HTTP was the local backend would stop being a usable oracle for the cached
# path (WORKSPACE.md section 2, invariant 5).

cmake_minimum_required(VERSION 3.23)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../VERSION" _usd_asset_cache_version
         LIMIT_COUNT 1)
    string(STRIP "${_usd_asset_cache_version}" _usd_asset_cache_version)
else()
    set(_usd_asset_cache_version "0.3.0")
endif()

project(usdAssetCache
    VERSION ${_usd_asset_cache_version}
    DESCRIPTION "Aligned block cache, coalescing, and single-flight over AssetReader"
    LANGUAGES CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED USDASSETCACHE_BUILD_TESTS)
    if(DEFINED USD_HTTP_RESOLVER_BUILD_TESTS)
        set(_usd_asset_cache_tests_default "${USD_HTTP_RESOLVER_BUILD_TESTS}")
    else()
        set(_usd_asset_cache_tests_default "${PROJECT_IS_TOP_LEVEL}")
    endif()
    option(USDASSETCACHE_BUILD_TESTS "Build usdAssetCache tests"
           ${_usd_asset_cache_tests_default})
endif()

# Standalone builds (`ost library build libs/usd-asset-cache`) resolve the
# dependency; in-tree builds already have the target from the root's ordered
# library list.
if(NOT TARGET usdasset::io)
    find_package(usdAssetIo CONFIG REQUIRED)
endif()

add_library(usdAssetCache STATIC
    src/BlockCache.cpp
    src/BlockPlan.cpp
    src/CacheKey.cpp
    src/CacheOptions.cpp
    src/CachedAssetReader.cpp
    src/DiskBlockStore.cpp)
add_library(usdasset::cache ALIAS usdAssetCache)
set_target_properties(usdAssetCache PROPERTIES
    EXPORT_NAME cache
    POSITION_INDEPENDENT_CODE ON)

target_compile_features(usdAssetCache PUBLIC cxx_std_17)
target_include_directories(usdAssetCache
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(usdAssetCache PUBLIC usdasset::io)

find_package(Threads REQUIRED)
target_link_libraries(usdAssetCache PUBLIC Threads::Threads)

# The persistent tier is std::filesystem. GCC before 9 keeps it in a separate
# library rather than in libstdc++, and the failure without this is a link error
# naming symbols nobody wrote.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(usdAssetCache PUBLIC stdc++fs)
endif()

if(MSVC)
    target_compile_options(usdAssetCache PRIVATE /utf-8 /W4)
else()
    target_compile_options(usdAssetCache PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS usdAssetCache
    EXPORT usdAssetCacheTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/usdAssetCacheConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetCacheConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetCache")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetCacheConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(EXPORT usdAssetCacheTargets
    FILE usdAssetCacheTargets.cmake
    NAMESPACE usdasset::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetCache")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetCacheConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/usdAssetCacheConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usdAssetCache")

if(USDASSETCACHE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
