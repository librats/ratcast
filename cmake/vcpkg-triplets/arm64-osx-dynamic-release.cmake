# arm64-osx with shared libraries and no debug builds: FFmpeg and the codecs a
# release bundles into ratcast.app, each a library of its own as LGPL wants
# (.github/workflows/release.yml), installed by @rpath for the bundle to carry.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_FIXUP_MACHO_RPATH ON)
