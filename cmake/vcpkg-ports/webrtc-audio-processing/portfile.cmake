# webrtc-audio-processing 1.3, the version Debian and Ubuntu ship
# (webrtc-audio-processing-1): 2.x has no voice detection, which the engine
# reports who is speaking by. Static, as its sources export nothing a DLL
# would need.

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_gitlab(
    GITLAB_URL https://gitlab.freedesktop.org
    OUT_SOURCE_PATH SOURCE_PATH
    REPO pulseaudio/webrtc-audio-processing
    REF v1.3
    SHA512 addd6feb5f46f958786b0befadbafe1247737a5cd002a631d9fb2c85fd121959287079148a73f3a20f2b594b288cd5697ab28f4063a4d421fbee9ed2f1ea2117
    HEAD_REF master
)

vcpkg_configure_meson(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_install_meson()
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
