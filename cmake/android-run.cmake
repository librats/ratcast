# Runs a program built for Android on a device through adb: what ctest and
# GoogleTest's discovery start an Android build's tests through
# (CMAKE_CROSSCOMPILING_EMULATOR, set in RatcastTesting.cmake).
#
#   cmake -DADB=<adb> -DSTRIP=<llvm-strip> -DLIBS=<a.so>|<b.so>...
#         -P android-run.cmake -- <program> [args...]
#
# The device is adb's: the only one there, or ANDROID_SERIAL's. A program and
# the shared libraries it needs (LIBS: libc++, FFmpeg's) are pushed only when
# they changed since they last were, as tests run one test case a process, the
# program stripped; a lock keeps the tests ctest runs side by side from pushing
# one at once. It runs in /data/local/tmp/ratcast with TMPDIR there.

set(dir /data/local/tmp/ratcast)

set(program "")
set(args "")
set(after_dashes FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE 1 ${last})
    set(arg "${CMAKE_ARGV${i}}")
    if(after_dashes)
        if(program STREQUAL "")
            set(program "${arg}")
        else()
            # Each argument quoted for the device's shell, ' as '\''.
            string(REPLACE "'" "'\\''" arg "${arg}")
            string(APPEND args " '${arg}'")
        endif()
    elseif(arg STREQUAL "--")
        set(after_dashes TRUE)
    endif()
endforeach()
if(program STREQUAL "")
    message(FATAL_ERROR "usage: cmake -DADB=... -P android-run.cmake -- <program> [args...]")
endif()
get_filename_component(name "${program}" NAME)

# Pushes `local` to `remote` unless the stamp says this device has it as it is.
function(push local remote stamp)
    file(TIMESTAMP "${local}" changed "%s" UTC)
    set(want "${serial} ${changed}")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" have)
        if(have STREQUAL want)
            return()
        endif()
    endif()
    execute_process(COMMAND "${ADB}" push "${local}" "${remote}"
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "adb push ${local}: ${error}")
    endif()
    file(WRITE "${stamp}" "${want}")
endfunction()

execute_process(COMMAND "${ADB}" shell "mkdir -p ${dir}/tmp && getprop ro.serialno"
    RESULT_VARIABLE result OUTPUT_VARIABLE serial OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "no device: adb shell failed")
endif()

set(stripped "${program}.android")
file(LOCK "${program}.android.lock" GUARD PROCESS TIMEOUT 600)
if(NOT EXISTS "${stripped}" OR "${program}" IS_NEWER_THAN "${stripped}")
    execute_process(COMMAND "${STRIP}" --strip-debug -o "${stripped}" "${program}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${STRIP} ${program} failed")
    endif()
endif()
push("${stripped}" "${dir}/${name}" "${program}.android.pushed")
execute_process(COMMAND "${ADB}" shell "chmod 755 ${dir}/${name}")
file(LOCK "${program}.android.lock" RELEASE)

get_filename_component(stamps "${program}" DIRECTORY)
string(REPLACE "|" ";" libs "${LIBS}")
foreach(lib IN LISTS libs)
    get_filename_component(lib_name "${lib}" NAME)
    file(LOCK "${stamps}/${lib_name}.android.lock" GUARD PROCESS TIMEOUT 600)
    push("${lib}" "${dir}/${lib_name}" "${stamps}/${lib_name}.android.pushed")
    file(LOCK "${stamps}/${lib_name}.android.lock" RELEASE)
endforeach()

execute_process(
    COMMAND "${ADB}" shell
        "cd ${dir} && LD_LIBRARY_PATH=${dir} TMPDIR=${dir}/tmp HOME=${dir}/tmp ./${name}${args}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    # cmake -P ends with 1 on an error, which is all ctest asks of a failure.
    message(FATAL_ERROR "${name} on the device ended with ${result}")
endif()
