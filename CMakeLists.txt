cmake_minimum_required(VERSION 3.25.0 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROJECT AmneziaVPN)
if(NOT AMNEZIAVPN_VERSION)
    set(AMNEZIAVPN_VERSION 5.0.3.1 CACHE STRING "Client app version")
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

if(APPLE AND NOT IOS)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(AMN_PF_RULE_IDENTITY "user { root }")
    else()
        set(AMN_PF_RULE_IDENTITY "group { amnvpn }")
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

# A separate installation that runs next to the regular AmneziaVPN, e.g.
# -DAMNEZIA_INSTANCE_ID=Test builds AmneziaVPNTest: its own app bundle,
# settings, keychain, service, IPC and daemon sockets and installer scripts
# (packaging: macOS). Empty for the regular build.
set(AMNEZIA_INSTANCE_ID "" CACHE STRING "Side-by-side installation id (empty for the regular build)")
set(AMNEZIA_APP_BUNDLE_NAME "AmneziaVPN${AMNEZIA_INSTANCE_ID}")
if(AMNEZIA_INSTANCE_ID)
    if(NOT AMNEZIA_INSTANCE_ID MATCHES "^[A-Za-z0-9]+$")
        message(FATAL_ERROR "AMNEZIA_INSTANCE_ID must be alphanumeric")
    endif()
    string(TOLOWER "${AMNEZIA_INSTANCE_ID}" AMNEZIA_INSTANCE_ID_LOWER)
    set(CLIENT_APPLICATION_NAME     "${AMNEZIA_APP_BUNDLE_NAME}")
    set(CLIENT_SERVICE_NAME         "${AMNEZIA_APP_BUNDLE_NAME}-service")
    set(CLIENT_ORGANIZATION_NAME    "${AMNEZIA_APP_BUNDLE_NAME}.ORG")
    set(CLIENT_APP_INSTANCE_NAME    "${AMNEZIA_APP_BUNDLE_NAME}Instance")
    # The updater would offer the regular release in its place.
    set(CLIENT_ENABLE_APP_UPDATES   0)
    add_compile_definitions(AMNEZIA_INSTANCE_SUFFIX="-${AMNEZIA_INSTANCE_ID_LOWER}")
    add_compile_definitions(AMNEZIA_INSTANCE_APP_NAME="${AMNEZIA_APP_BUNDLE_NAME}")
    message(STATUS "Side-by-side installation: ${AMNEZIA_APP_BUNDLE_NAME}")
endif()

add_subdirectory(client)

if(NOT IOS AND NOT ANDROID AND NOT MACOS_NE)
    add_subdirectory(service)
endif()

if ((LINUX AND NOT ANDROID) OR (APPLE AND NOT IOS AND NOT MACOS_NE) OR (WIN32))
    include(${CMAKE_SOURCE_DIR}/cmake/CPack.cmake)
endif()
