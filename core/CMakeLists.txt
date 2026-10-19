# pbc_core: Qt-free core library (transports, storage, sync helpers). It MUST NOT depend on Qt or InkView
# (constitution Principle II); it builds on the host without the SDK and, for the device, with the SDK toolchain.

# Vendored SQLite amalgamation (pinned, see third_party/sqlite/VERSION).
add_library(pbc_sqlite STATIC ${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c)
target_include_directories(pbc_sqlite PUBLIC ${CMAKE_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(pbc_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED
    SQLITE_ENABLE_FTS5)
target_compile_options(pbc_sqlite PRIVATE -w)
target_link_libraries(pbc_sqlite PUBLIC pthread m)

set(PBC_CORE_SOURCES
    src/version.cpp
    src/types.cpp
    src/curl_session.cpp
    src/curl_transport.cpp
    src/http_transport.cpp
    src/dav_common.cpp
    src/xml_min.cpp
    src/ca_selector.cpp
    src/credentials_file.cpp
    src/store.cpp
    src/store_migrations.cpp
    src/store_state.cpp
    src/store_util.cpp
    src/log.cpp
    src/utf8.cpp
    src/tz.cpp
    src/clock_check.cpp
    src/paging.cpp
    src/sync_scheduler.cpp
    src/about.cpp
)

# libcurl: the system package on the host (8.18.0 or newer), the bundled static build for the device.
if(TARGET_TYPE STREQUAL "ARM")
    include(${CMAKE_SOURCE_DIR}/cmake/curl_arm.cmake)
else()
    find_package(CURL REQUIRED)
endif()

add_library(pbc_core STATIC ${PBC_CORE_SOURCES})
target_include_directories(pbc_core PUBLIC include)
target_compile_features(pbc_core PUBLIC cxx_std_17)
target_compile_definitions(pbc_core PRIVATE PBC_VERSION="${PBC_VERSION}")
target_link_libraries(pbc_core PUBLIC pbc_sqlite CURL::libcurl)
