# Monocypher 4.0.2 (https://monocypher.org, BSD-2-Clause or CC0-1.0, LICENCE.md):
# src/monocypher.c and src/monocypher.h of its release, as they are. The
# client's identities sign with its EdDSA (Ed25519 over BLAKE2b) and MAC with
# keyed BLAKE2b (client/src/identity.cpp).
add_library(monocypher STATIC monocypher.c monocypher.h)
target_include_directories(monocypher SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)
