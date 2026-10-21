# The Qt Quick application. ARM: SDK Qt via absolute library paths, manual moc/rcc. HOST: host Qt6
# when the Qml/Quick dev packages are installed (otherwise only the core library and tests are built).

file(GLOB PBC_QML_FILES CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/qml/*.qml ${CMAKE_SOURCE_DIR}/qml/pages/*.qml
    ${CMAKE_SOURCE_DIR}/qml/qmldir ${CMAKE_SOURCE_DIR}/qml/icons/*.svg)
set(PBC_APP_SOURCES main.cpp device_bridge.cpp)

# The device probe has no Qt dependency and builds on every target.
add_subdirectory(probe)

if(TARGET_TYPE STREQUAL "ARM")
    pbc_moc(PBC_MOC_SOURCES device_bridge.h)
    pbc_rcc(PBC_QML_RCC qml ${CMAKE_SOURCE_DIR}/qml/qml.qrc ${PBC_QML_FILES})

    add_executable(pbcloud_app ${PBC_APP_SOURCES} ${PBC_MOC_SOURCES} ${PBC_QML_RCC})
    target_include_directories(pbcloud_app SYSTEM PRIVATE ${PBC_QT_INCLUDES})
    target_include_directories(pbcloud_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(pbcloud_app PRIVATE PLATFORM_POCKETBOOK PBC_VERSION="${PBC_VERSION}")
    # libinkview lives outside the default sysroot library directories.
    target_link_directories(pbcloud_app PRIVATE ${SYSROOT}/usr/local/lib)
    target_link_libraries(pbcloud_app PRIVATE pbc_bridge ${PBC_QT_LIBS} pthread inkview)
    # Qt's own dependencies and libcurl's are found next to them at link time.
    target_link_options(pbcloud_app PRIVATE -Wl,-rpath-link,${QT6_ROOT}/lib -Wl,-rpath-link,${SYSROOT}/usr/lib)
    # The ELF is installed as <app>.bin and a shell launcher is installed as <app>.app.
    set_target_properties(pbcloud_app PROPERTIES SUFFIX ".app" OUTPUT_NAME "${PBC_APP_NAME}")
else()
    find_package(Qt6 QUIET COMPONENTS Core Gui Qml Quick Test)
    if(Qt6Quick_FOUND AND Qt6Qml_FOUND AND TARGET pbc_bridge)
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        add_executable(pbcloud_app ${PBC_APP_SOURCES} ui_script.cpp ${CMAKE_SOURCE_DIR}/qml/qml.qrc)
        set_property(SOURCE ${CMAKE_SOURCE_DIR}/qml/qml.qrc PROPERTY OBJECT_DEPENDS ${PBC_QML_FILES})
        target_include_directories(pbcloud_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(pbcloud_app PRIVATE
            PBC_VERSION="${PBC_VERSION}"
            PBC_BUNDLED_CA="${CMAKE_SOURCE_DIR}/third_party/cacert/cacert.pem"
            PBC_ZONEINFO_DIR="${CMAKE_SOURCE_DIR}/third_party/tzdata/zoneinfo"
            PBC_LICENSES_ROOT="${CMAKE_SOURCE_DIR}"
            PBC_EMOJI_FONT="${CMAKE_SOURCE_DIR}/third_party/noto-emoji/NotoEmoji.ttf")
        target_link_libraries(pbcloud_app PRIVATE pbc_bridge Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)   # Qt6::Test only for the host walk (ui_script.cpp)
        set_target_properties(pbcloud_app PROPERTIES OUTPUT_NAME "${PBC_APP_NAME}")
        # The UI must load without QML errors (offscreen platform, no display needed).
        add_test(NAME app_self_test COMMAND pbcloud_app --self-test)
        set_tests_properties(app_self_test PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" LABELS "bridge" WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        if(PBC_SANITIZE)
            set_property(TEST app_self_test APPEND PROPERTY ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.supp:print_suppressions=0")
        endif()
        # The scripted walk through the real pages against the fake Nextcloud (scripts/ui-walk.sh): the acceptance walk of the files client.
        find_program(PBC_OPENSSL_CLI openssl)
        if(TARGET pbc_fake_nextcloud AND PBC_OPENSSL_CLI)
            add_test(NAME ui_walk COMMAND ${CMAKE_SOURCE_DIR}/scripts/ui-walk.sh --out ${CMAKE_BINARY_DIR}/walk-out --build ${CMAKE_BINARY_DIR}
                ${CMAKE_SOURCE_DIR}/tests/walks/01-connect-browse-download.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/02-restart-update-favorites-search-storage.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/03-upload-and-manage.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/04-unknown-outcome.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/05-collisions-and-conflicts.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/06-new-folder-and-rename.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/07-move-and-copy.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/08-delete.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/09-refusals.walk
                ${CMAKE_SOURCE_DIR}/tests/walks/10-no-trash.walk)
            set_tests_properties(ui_walk PROPERTIES LABELS "bridge" TIMEOUT 900)
            if(PBC_SANITIZE)
                set_property(TEST ui_walk APPEND PROPERTY ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.supp:print_suppressions=0")
            endif()
        endif()
    else()
        message(STATUS "Qt6 Qml/Quick dev packages not found - building the core library and tests only")
    endif()
endif()
