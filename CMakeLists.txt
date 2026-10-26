cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROJECT AmneziaVPN)
if(NOT AMNEZIAVPN_VERSION)
    set(AMNEZIAVPN_VERSION 5.0.3.1 CACHE STRING "Client app version")
endif()
# Fork (Лёша VPN) release number on top of the upstream base version above: bump it by 1 for every
# fork release and never reset it (not even when merging a new upstream AMNEZIAVPN_VERSION), because
# the Android versionCode, the MSI version and the Apple build number are derived from it.
if(NOT LESHA_VERSION)
    set(LESHA_VERSION 8 CACHE STRING "Fork release number (bump by 1 per release, never reset)")
endif()

set(QT_CREATOR_SKIP_PACKAGE_MANAGER_SETUP ON CACHE BOOL "" FORCE)
set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES
    ${CMAKE_SOURCE_DIR}/cmake/platform_settings.cmake
    ${CMAKE_SOURCE_DIR}/cmake/recipes_bootstrap.cmake
    ${CMAKE_SOURCE_DIR}/cmake/conan_provider.cmake
    CACHE STRING "" FORCE)

project(${PROJECT} VERSION ${AMNEZIAVPN_VERSION}
        DESCRIPTION "AmneziaVPN"
        HOMEPAGE_URL "https://amnezia.org/"
)

option(AMNEZIA_BUILD_TESTS "Build test executables" OFF)

# trigger conan to kick off `conan install` globally
find_package(OpenSSL REQUIRED)
if (PREBUILTS_ONLY)
    return()
endif()

string(TIMESTAMP CURRENT_DATE "%Y-%m-%d")
set(RELEASE_DATE "${CURRENT_DATE}")

set(APP_MAJOR_VERSION ${CMAKE_PROJECT_VERSION_MAJOR}.${CMAKE_PROJECT_VERSION_MINOR}.${CMAKE_PROJECT_VERSION_PATCH})

# bump by 2 on every release, because we're releasing two versions on the play store
set(APP_ANDROID_VERSION_CODE 2171)
if(DEFINED APP_ANDROID_VERSION_CODE_OFFSET)
    math(EXPR APP_ANDROID_VERSION_CODE "${APP_ANDROID_VERSION_CODE} + ${APP_ANDROID_VERSION_CODE_OFFSET}")
endif()

# === Fork versions: the only place they are computed ===
# LESHA_FULL_VERSION    "5.0.3.1-lesha.N": shown in the UI, X-App-Version, Android versionName,
#                       package file names, release tag (v${LESHA_FULL_VERSION})
# LESHA_NUMERIC_VERSION "MAJOR.MINOR.(PATCH*1000+N)": MSI/CPack package version (MSI only compares
#                       the first three fields, so the fork number has to live in the third one)
# LESHA_RC_VERSION      "MAJOR,MINOR,PATCH*1000+N,TWEAK": Windows .rc FILEVERSION/PRODUCTVERSION
# LESHA_ANDROID_VERSION_CODE  upstream code (incl. APP_ANDROID_VERSION_CODE_OFFSET) * 100 + N
# LESHA_APPLE_BUILD     TWEAK*1000 + N: CFBundleVersion / CURRENT_PROJECT_VERSION
if(NOT LESHA_VERSION MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "LESHA_VERSION must be a positive integer, got '${LESHA_VERSION}'")
endif()
if(LESHA_VERSION GREATER_EQUAL 100)
    message(FATAL_ERROR "LESHA_VERSION=${LESHA_VERSION} does not fit into the Android versionCode "
        "(upstream code * 100 + LESHA_VERSION, so it must stay below 100). Bump the upstream base "
        "version (merge upstream) and rethink the versioning scheme here, e.g. a bigger multiplier: "
        "every new versionCode must still exceed all codes already published.")
endif()
set(_lesha_tweak "${CMAKE_PROJECT_VERSION_TWEAK}")
if(_lesha_tweak STREQUAL "")
    set(_lesha_tweak 0)
endif()
# cached (INTERNAL) so CI can read it from CMakeCache.txt to name artifacts
set(LESHA_FULL_VERSION "${AMNEZIAVPN_VERSION}-lesha.${LESHA_VERSION}" CACHE INTERNAL "Full fork version")
math(EXPR _lesha_patch "${CMAKE_PROJECT_VERSION_PATCH} * 1000 + ${LESHA_VERSION}")
if(_lesha_patch GREATER 65535)
    message(FATAL_ERROR "PATCH*1000+LESHA_VERSION = ${_lesha_patch} exceeds 65535 (MSI version field limit)")
endif()
set(LESHA_NUMERIC_VERSION "${CMAKE_PROJECT_VERSION_MAJOR}.${CMAKE_PROJECT_VERSION_MINOR}.${_lesha_patch}")
set(LESHA_RC_VERSION "${CMAKE_PROJECT_VERSION_MAJOR},${CMAKE_PROJECT_VERSION_MINOR},${_lesha_patch},${_lesha_tweak}")
math(EXPR LESHA_ANDROID_VERSION_CODE "${APP_ANDROID_VERSION_CODE} * 100 + ${LESHA_VERSION}")
math(EXPR LESHA_APPLE_BUILD "${_lesha_tweak} * 1000 + ${LESHA_VERSION}")
message(STATUS "Version ${LESHA_FULL_VERSION} (numeric ${LESHA_NUMERIC_VERSION}, "
    "android code ${LESHA_ANDROID_VERSION_CODE}, apple build ${LESHA_APPLE_BUILD})")

if(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(MZ_PLATFORM_NAME "linux")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(MZ_PLATFORM_NAME "windows")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(MZ_PLATFORM_NAME "macos")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    set(MZ_PLATFORM_NAME "android")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "iOS")
    set(MZ_PLATFORM_NAME "ios")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Emscripten")
    set(MZ_PLATFORM_NAME "wasm")
endif()

set(QT_BUILD_TOOLS_WHEN_CROSS_COMPILING ON)

# Branding (app/service names etc.) is needed here already for the macOS service group and by
# cmake/CPack.cmake; client/CMakeLists.txt includes it again (the defaults are idempotent).
include(${CMAKE_CURRENT_SOURCE_DIR}/client/cmake/clientBrandingDefaults.cmake)

# macOS group the service runs as (launchd plist GroupName) and that PF lets through;
# per-brand so uninstalling one client doesn't delete the other's group
string(TOLOWER "${CLIENT_APPLICATION_NAME}" AMN_SERVICE_GROUP)

if(APPLE AND NOT IOS)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(AMN_PF_RULE_IDENTITY "user { root }")
    else()
        set(AMN_PF_RULE_IDENTITY "group { ${AMN_SERVICE_GROUP} }")
    endif()

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/deploy/data/pf-templates/amn.400.allowPIA.conf.in"
        "${CMAKE_CURRENT_BINARY_DIR}/amn.400.allowPIA.conf"
        @ONLY
    )

    file(COPY_FILE
        "${CMAKE_CURRENT_BINARY_DIR}/amn.400.allowPIA.conf"
        "${CMAKE_CURRENT_SOURCE_DIR}/deploy/data/macos/pf/amn.400.allowPIA.conf"
        ONLY_IF_DIFFERENT
    )
endif()


add_subdirectory(client)

if(NOT IOS AND NOT ANDROID AND NOT MACOS_NE)
    add_subdirectory(service)
endif()

if ((LINUX AND NOT ANDROID) OR (APPLE AND NOT IOS AND NOT MACOS_NE) OR (WIN32))
    include(${CMAKE_SOURCE_DIR}/cmake/CPack.cmake)
endif()
