# Libs-first root for the usd-http-resolver workspace.
#
# The build graph mirrors the dependency boundary fixed in
# docs/architecture/WORKSPACE.md: everything under libs/ is transport- and
# OpenUSD-agnostic and builds with plain CMake alone; only the OpenStrata
# plugin bundles under plugins/ need an OpenUSD runtime. OpenUSD is therefore
# resolved *inside* the plugin branch, never at the top of this file.
#
#   # core only -- no OpenUSD installation required
#   cmake -S . -B build-core -DUSD_HTTP_RESOLVER_BUILD_PLUGIN=OFF
#   cmake --build build-core
#   ctest --test-dir build-core
#
#   # everything, including the ArResolver bundle
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=<your-openusd-install>
#   cmake --build build
#
# `ost` builds each bundle individually (`ost plugin build <bundle>`),
# composing the resolved runtime's environment and toolchain. Each bundle
# guards its own `find_package(pxr)` with `if(NOT pxr_FOUND)`, so it builds
# either standalone (via `ost`) or as a subdirectory of this root.
cmake_minimum_required(VERSION 3.23)
project(usd-http-resolver LANGUAGES CXX)

option(USD_HTTP_RESOLVER_BUILD_PLUGIN
       "Build the OpenUSD ArResolver plugin bundles under plugins/" ON)

enable_testing()

# --- libs/ -------------------------------------------------------------------
#
# Always built, never dependent on OpenUSD. The list is the landing order from
# the roadmap, not a directory listing: a module is added only once it exists,
# because a directory is created when its first tested capability exists
# (WORKSPACE.md invariant 11). The glob below adds nothing -- it exists so that
# `CONFIGURE_DEPENDS` re-runs configuration when a new module lands.
file(GLOB _usd_http_resolver_lib_stamps CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/libs/*/CMakeLists.txt")

set(_usd_http_resolver_libs
    usd-asset-io
    usd-asset-local
    usd-asset-http
    usd-asset-cache)

foreach(_lib ${_usd_http_resolver_libs})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/${_lib}/CMakeLists.txt")
        message(STATUS "usd-http-resolver: adding library 'libs/${_lib}'")
        add_subdirectory("libs/${_lib}")
    endif()
endforeach()

# --- plugins/ ----------------------------------------------------------------
#
# The only branch that requires OpenUSD. A bundle is any directory containing
# both an openstrata.plugin.yaml and a CMakeLists.txt, either an immediate
# subdirectory of this root or one under plugins/. `CONFIGURE_DEPENDS` makes the
# build re-glob when a bundle is added or removed, so a later `cmake --build`
# picks up new `ost plugin new` bundles without a manual reconfigure.
if(NOT USD_HTTP_RESOLVER_BUILD_PLUGIN)
    message(STATUS
            "usd-http-resolver: plugin bundles disabled; OpenUSD is not required")
    return()
endif()

if(NOT pxr_FOUND)
    find_package(pxr CONFIG REQUIRED)
endif()

set(_ost_bundle_roots "${CMAKE_CURRENT_SOURCE_DIR}")
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
    list(APPEND _ost_bundle_roots "${CMAKE_CURRENT_SOURCE_DIR}/plugins")
endif()

foreach(_root ${_ost_bundle_roots})
    file(GLOB _entries LIST_DIRECTORIES true CONFIGURE_DEPENDS RELATIVE
         "${_root}" "${_root}/*")
    foreach(_entry ${_entries})
        set(_bundle "${_root}/${_entry}")
        if(IS_DIRECTORY "${_bundle}"
           AND EXISTS "${_bundle}/openstrata.plugin.yaml"
           AND EXISTS "${_bundle}/CMakeLists.txt")
            file(RELATIVE_PATH _rel_bundle "${CMAKE_CURRENT_SOURCE_DIR}" "${_bundle}")
            message(STATUS "OpenStrata: adding plugin bundle '${_rel_bundle}'")
            add_subdirectory("${_rel_bundle}")
        endif()
    endforeach()
endforeach()
