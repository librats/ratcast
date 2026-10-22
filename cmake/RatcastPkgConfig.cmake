# pkg-config's findings for every build configuration at once.
#
# A multi-config generator (Visual Studio) configures once for Debug and
# Release both, and pkg-config finds one build of each library: vcpkg's debug
# one, its .pc files coming first on the path. A Release build must link the
# release one instead — MSVC's two C++ runtimes do not mix in one binary — so
# each library vcpkg keeps in both builds is made to follow the configuration.

# ratcast_pkg_config_per_config(<imported target>)
function(ratcast_pkg_config_per_config target)
    get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT multi_config)
        return()
    endif()
    get_target_property(libraries ${target} INTERFACE_LINK_LIBRARIES)
    if(NOT libraries)
        return()
    endif()
    set(per_config)
    foreach(library IN LISTS libraries)
        # vcpkg keeps <prefix>/lib/<name> and <prefix>/debug/lib/<name>.
        set(debug "")
        set(release "")
        if(library MATCHES "^(.*)/debug/lib/([^/]+)$")
            set(debug "${library}")
            set(release "${CMAKE_MATCH_1}/lib/${CMAKE_MATCH_2}")
        elseif(library MATCHES "^(.*)/lib/([^/]+)$")
            set(debug "${CMAKE_MATCH_1}/debug/lib/${CMAKE_MATCH_2}")
            set(release "${library}")
        endif()
        if(debug AND EXISTS "${debug}" AND EXISTS "${release}")
            set(library "$<IF:$<CONFIG:Debug>,${debug},${release}>")
        endif()
        list(APPEND per_config "${library}")
    endforeach()
    set_target_properties(${target} PROPERTIES INTERFACE_LINK_LIBRARIES "${per_config}")
endfunction()
