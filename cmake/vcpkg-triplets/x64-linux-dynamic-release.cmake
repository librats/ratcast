# x64-linux with shared libraries and no debug builds: FFmpeg and the codecs a
# release bundles into its AppImage, each a library of its own as LGPL wants
# (.github/workflows/release.yml).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_FIXUP_ELF_RPATH ON)
