# pbc-probe: a console program (libcurl, pbc_core and the C library only, no Qt, no InkView) that prints what the
# reader's runtime can do (research R-02). It builds on the host (tests/launcher/test_probe.sh) and for the
# device with the SDK toolchain, where it is installed as pbc-probe.bin next to the app.
if(NOT TARGET CURL::libcurl)
    find_package(CURL REQUIRED)
endif()
add_executable(pbc-probe pbc_probe.cpp)
target_compile_features(pbc-probe PRIVATE cxx_std_17)
target_link_libraries(pbc-probe PRIVATE pbc_core CURL::libcurl)
if(TARGET_TYPE STREQUAL "ARM")
    set_target_properties(pbc-probe PROPERTIES SUFFIX ".bin")
endif()
