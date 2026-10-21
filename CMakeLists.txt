cmake_minimum_required(VERSION 3.16)

# Build target:
#   HOST (default) - desktop build: the Qt-free core library and its tests; the Qt app
#                    is built too when the Qt6 Qml/Quick dev packages are installed.
#   ARM            - PocketBook device build, cross-compiled with the PocketBook SDK.
#                    The SDK location comes ONLY from -DSDK_PATH or $POCKETBOOK_SDK.
set(TARGET_TYPE "HOST" CACHE STRING "Build target: HOST (desktop, tests) or ARM (PocketBook device)")
set_property(CACHE TARGET_TYPE PROPERTY STRINGS HOST ARM)
if(NOT TARGET_TYPE STREQUAL "HOST" AND NOT TARGET_TYPE STREQUAL "ARM")
    message(FATAL_ERROR "TARGET_TYPE must be HOST or ARM (got '${TARGET_TYPE}')")
endif()

# The toolchain has to be selected before project().
if(TARGET_TYPE STREQUAL "ARM")
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/sdk.cmake)
endif()

# The release version lives in the VERSION file (single source of truth for the app and the package).
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/VERSION)
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/VERSION PBC_FILE_VERSION LIMIT_COUNT 1)
if(NOT PBC_FILE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "VERSION must start with MAJOR.MINOR.PATCH (got '${PBC_FILE_VERSION}')")
endif()

project(pbcloud VERSION ${PBC_FILE_VERSION} LANGUAGES C CXX)

set(PBC_APP_NAME "pbcloud" CACHE STRING "Application/binary base name")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Version string shown in the app: the VERSION file; override with -DPBC_VERSION=...
if(NOT PBC_VERSION)
    set(PBC_VERSION "${PBC_FILE_VERSION}")
endif()
message(STATUS "pbcloud version: ${PBC_VERSION} (${TARGET_TYPE})")

add_compile_options(-Wall -Wextra)
# Files of 2 GiB up to the 4 GiB limit of FAT: 64-bit file offsets also on the 32-bit device (uploads read chunks of such files by offset).
add_compile_definitions(_FILE_OFFSET_BITS=64)

# Sanitizer build for the parsers and the interruption tests (Principle I, SC-010): cmake -DPBC_SANITIZE=ON
option(PBC_SANITIZE "Build everything with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(PBC_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# Qt-free core library (transports, storage, sync helpers): builds and tests on the host
# without Qt, InkView or the SDK (constitution Principle II).
add_subdirectory(core)

# Qt layer (bridge) and the application; the manual moc/rcc helpers are only needed for the device.
if(TARGET_TYPE STREQUAL "ARM")
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/qt_manual.cmake)
endif()
add_subdirectory(bridge)

if(NOT TARGET_TYPE STREQUAL "ARM")
    enable_testing()
    add_subdirectory(tests)
endif()

# The Qt Quick application.
add_subdirectory(app)
