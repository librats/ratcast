# QR Code generator library 1.8.0, C++ (Project Nayuki, MIT License: in each
# file's header): cpp/qrcodegen.cpp and cpp/qrcodegen.hpp of its release, as
# they are. The app draws links as QR codes with it (app/call/qr_code.h).
add_library(qrcodegen STATIC qrcodegen.cpp qrcodegen.hpp)
target_include_directories(qrcodegen SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(qrcodegen PROPERTIES POSITION_INDEPENDENT_CODE ON)
