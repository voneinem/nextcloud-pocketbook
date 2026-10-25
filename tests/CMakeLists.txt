# Host-only tests. Every tests/{unit,integration}/test_*.cpp becomes its own
# executable (linked with pbc_core) registered with CTest and labelled by directory.
# Tests are written first and must fail before the implementation exists (Principle I).
# tests/templates/ is NOT globbed: it keeps the performance harness of the notes baseline as a template for the budget tests.

file(GLOB PBC_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_*.cpp)

# The TLS tests run local HTTPS servers with libssl and need the `openssl` CLI at run time.
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

foreach(src ${PBC_TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    get_filename_component(dir ${src} DIRECTORY)
    get_filename_component(kind ${dir} NAME)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE pbc_core OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_compile_definitions(${name} PRIVATE
        PBC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        PBC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${kind}" SKIP_RETURN_CODE 77)
    if(PBC_SANITIZE)
        # leaks inside system libraries loaded by libcurl are not ours (tests/lsan.supp)
        set_property(TEST ${name} APPEND PROPERTY ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp:print_suppressions=0")
    endif()
endforeach()

# A fake Nextcloud over HTTPS for trying the host app by hand (scripts/run-host-demo.sh); not a test.
add_executable(pbc_fake_nextcloud tools/pbc_fake_nextcloud.cpp)
target_link_libraries(pbc_fake_nextcloud PRIVATE pbc_core OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
target_include_directories(pbc_fake_nextcloud PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)

# Mechanical anonymization check of the fixtures, and a self-test proving the checker works.
add_test(NAME check_fixtures
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-fixtures.sh ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
add_test(NAME check_fixtures_selftest
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-fixtures.sh --self-test)
add_test(NAME check_constitution COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-constitution.sh ${CMAKE_SOURCE_DIR})
add_test(NAME check_constitution_selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-constitution.sh --self-test)
# No bordered text button anywhere in the QML, and a self-test proving the checker works.
add_test(NAME check_ui_style COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-ui-style.sh ${CMAKE_SOURCE_DIR})
add_test(NAME check_ui_style_selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-ui-style.sh --self-test)
# Every text of the QML has a German translation with the same placeholders, and the table has no stale entries (T175).
add_test(NAME check_translations COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/extract-strings.py --check)
set_tests_properties(check_translations PROPERTIES LABELS "script")
add_test(NAME package_size_selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/package-device.sh --self-test)
set_tests_properties(package_size_selftest PROPERTIES LABELS "script")
add_test(NAME check_version_bump_selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-version-bump.sh --self-test)
# Spec 007: tag, VERSION, package name and manifest version agree; a self-test proving the gate works.
add_test(NAME check_release_selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-release.sh --self-test)
set_tests_properties(check_release_selftest PROPERTIES LABELS "script")
set_tests_properties(check_fixtures check_fixtures_selftest check_constitution check_constitution_selftest
                     PROPERTIES LABELS "script")
add_test(NAME launcher_script COMMAND ${CMAKE_SOURCE_DIR}/tests/launcher/test_launcher.sh ${CMAKE_SOURCE_DIR})
# Spec 007: the release package follows the PB Store contract (manifest, store tool, reproducible, checked).
# Exit 77 = nothing could run (skipped).
add_test(NAME packaging_script COMMAND ${CMAKE_SOURCE_DIR}/tests/packaging/test_package_device.sh ${CMAKE_SOURCE_DIR})
set_tests_properties(packaging_script PROPERTIES SKIP_RETURN_CODE 77 LABELS "script")
add_test(NAME probe_script COMMAND ${CMAKE_SOURCE_DIR}/tests/launcher/test_probe.sh ${CMAKE_SOURCE_DIR} $<TARGET_FILE:pbc-probe>)
add_test(NAME check_fake_markers COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-fake-markers.sh ${CMAKE_SOURCE_DIR})
add_test(NAME check_fake_markers_selftest COMMAND ${CMAKE_SOURCE_DIR}/scripts/check-fake-markers.sh --self-test)
set_tests_properties(check_fake_markers check_fake_markers_selftest PROPERTIES LABELS "script")

# Bridge tests (Qt Core + the real core): only when the Qt layer is built.
if(TARGET pbc_bridge)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        foreach(t test_about_bridge test_table_translator test_settings_bridge test_connection_bridge test_browse_bridge test_transfer_bridge test_space_bridge test_no_background_network test_picker_bridge test_queue_bridge test_folder_chooser_bridge test_sync_bridge test_share_bridge test_mail_link test_trash_bridge test_versions_bridge)
            add_executable(${t} bridge/${t}.cpp)
            target_link_libraries(${t} PRIVATE pbc_bridge Qt6::Core Qt6::Test OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
            target_include_directories(${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
            target_compile_definitions(${t} PRIVATE PBC_SOURCE_DIR="${CMAKE_SOURCE_DIR}" PBC_LICENSES_ROOT="${CMAKE_SOURCE_DIR}" PBC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
            add_test(NAME ${t} COMMAND ${t})
            set_tests_properties(${t} PROPERTIES LABELS "bridge" TIMEOUT 120 SKIP_RETURN_CODE 77 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endforeach()
    else()
        message(STATUS "Qt6 Test not found - the bridge tests are not built")
    endif()
endif()

if(PBC_SANITIZE)
    foreach(t test_about_bridge test_table_translator test_settings_bridge test_connection_bridge test_browse_bridge test_transfer_bridge test_space_bridge test_no_background_network test_picker_bridge test_queue_bridge test_folder_chooser_bridge test_sync_bridge test_share_bridge test_mail_link test_trash_bridge test_versions_bridge probe_script)
        if(TEST ${t})
            set_property(TEST ${t} APPEND PROPERTY ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp:print_suppressions=0")
        endif()
    endforeach()
endif()
