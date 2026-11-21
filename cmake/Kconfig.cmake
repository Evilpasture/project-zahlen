# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/Kconfig.cmake
#
# A .config file as the source of cache values for this build.
#
# The TUI is `menuconfig` from Kconfiglib, wired in as a CMake preset
# (CMakePresets.json) and scripted by scripts/menuconfig.sh. It writes
# ${CMAKE_BINARY_DIR}/.config, and this module reads it at configure time and
# turns each `CONFIG_<NAME>=<value>` line into a cache entry -- but only for the
# symbols this project actually declares.
#
# Three rules, and each is a decision rather than an accident:
#
# 1. THE FILE WINS ONCE, THEN THE CACHE WINS.
#
#    option() writes its default into the cache on the first configure, so by the
#    time this module runs every variable is already cached and a plain
#    "do not overwrite the cache" rule would make the menu do nothing at all --
#    the exact bug this module was written with first. The two cases have to be
#    told apart:
#
#      the file changed since we last applied it  -> apply it, force the values
#      the file is unchanged                      -> leave the cache alone, so
#                                                    -D still works and a value
#                                                    set in ccmake sticks
#
#    The record of what was applied is a stamp file beside the .config, holding a
#    hash of the file's contents. Not a CMake variable: a variable in the cache
#    would be one more thing to keep in sync, and a plain variable would forget
#    itself at the top of every configure -- which is exactly the reconfigure this
#    has to survive.
#
# 2. IT IS OPT-IN. Nothing here runs unless ZHLN_KCACHE_CONFIG points at a file
#    (the preset sets it). A tree configured by hand, by CI or by an IDE never
#    reads a .config and never picks up a file someone left in a build directory.
#
# 3. A STALE KEY IS AN ERROR. If the .config names a symbol this CMakeLists no
#    longer declares, configure stops. Silently ignoring it would leave a menu
#    entry that appears to work and does nothing, which is the failure mode the
#    Kconfig front end exists to avoid.
#
#    "Declares" is lane-sensitive. Some entries only exist in some configurations
#    -- ZHLN_SLANG_* is declared inside include(ShaderCompilation), which the root
#    only includes when ZHLN_BUILD_RENDER is on. The second argument names those,
#    so turning the renderer off does not make the menu look stale.
#
# Booleans map to ON/OFF. Everything else is passed through as its literal
# string, so an int or a path set through the menu arrives as one.

include_guard(GLOBAL)

# zhln_kconfig_apply(<config file> [<lane symbols>...])
#
# Applies <config file> to the cache. Called from the root CMakeLists once the
# option() declarations exist, because a variable that is not yet declared
# cannot be told apart from a stale key.
#
# <lane symbols> names entries the menu may carry that this lane does not
# declare. They are skipped rather than set or reported: a variable nothing reads
# is not a configure failure, it is simply not part of this configuration.
function(zhln_kconfig_apply CONFIG_FILE)
    if(NOT EXISTS "${CONFIG_FILE}")
        message(FATAL_ERROR
            "ZHLN_KCACHE_CONFIG points at '${CONFIG_FILE}', which does not exist. "
            "Run ./scripts/menuconfig.sh to create it, or unset ZHLN_KCACHE_CONFIG "
            "to configure without a .config.")
    endif()

    set(_lane ${ARGN})
    set(_record "")
    set(_stale "")

    file(STRINGS "${CONFIG_FILE}" _lines)
    foreach(_line IN LISTS _lines)
        # `# CONFIG_FOO is not set` is an unset symbol, not a comment, and it has
        # to be honoured: the whole point of the menu is that turning something
        # off sticks.
        if(_line MATCHES "^# CONFIG_([A-Za-z0-9_]+) is not set")
            set(_name "${CMAKE_MATCH_1}")
            set(_value "OFF")
        elseif(_line MATCHES "^CONFIG_([A-Za-z0-9_]+)=(.*)$")
            set(_name "${CMAKE_MATCH_1}")
            set(_value "${CMAKE_MATCH_2}")
            # y/n are Kconfig's booleans; anything else is a literal.
            if(_value STREQUAL "y")
                set(_value "ON")
            elseif(_value STREQUAL "n")
                set(_value "OFF")
            endif()
        else()
            continue()
        endif()

        if(NOT DEFINED CACHE{${_name}} AND NOT DEFINED ${_name})
            # Absent because this lane does not build it, or absent because the
            # menu invented it. Only the second is a mistake.
            list(FIND _lane "${_name}" _lane_idx)
            if(_lane_idx EQUAL -1)
                list(APPEND _stale "${_name}")
            endif()
            continue()
        endif()

        list(APPEND _record "${_name}=${_value}")
    endforeach()

    if(_stale)
        list(JOIN _stale ", " _stale_str)
        message(FATAL_ERROR
            "${CONFIG_FILE} names symbols this tree does not declare: ${_stale_str}.\n"
            "Either the Kconfig menu is out of date with CMakeLists.txt, or the .config "
            "was written by a different checkout. Delete ${CONFIG_FILE} and run "
            "./scripts/menuconfig.sh again.")
    endif()

    # Rule 1: apply on change, then leave the cache alone. See the note above for
    # why a bare "the cache wins" rule is not enough.
    string(SHA256 _digest "${_record}")
    set(_stamp "${CONFIG_FILE}.stamp")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _previous)
        string(STRIP "${_previous}" _previous)
        if(_previous STREQUAL _digest)
            message(STATUS "Kconfig: ${CONFIG_FILE} unchanged; cached values win")
            return()
        endif()
    endif()
    file(WRITE "${_stamp}" "${_digest}\n")

    foreach(_entry IN LISTS _record)
        string(REGEX REPLACE "^([A-Za-z0-9_]+)=.*$" "\\1" _name "${_entry}")
        string(REGEX REPLACE "^[A-Za-z0-9_]+=(.*)$" "\\1" _value "${_entry}")
        if(DEFINED CACHE{${_name}})
            # Keep the declaration's own type. option() creates BOOL entries, and
            # re-declaring one as STRING would turn a checkbox into a text field
            # in every IDE that reads CMakeCache.txt, and make `if()` compare a
            # string the caller did not write.
            get_property(_type CACHE ${_name} PROPERTY TYPE)
            if(NOT _type)
                set(_type STRING)
            endif()
            set(${_name} "${_value}" CACHE ${_type} "" FORCE)
        else()
            set(${_name} "${_value}" CACHE STRING
                "Set by the Kconfig menu (${CONFIG_FILE})")
        endif()
    endforeach()

    list(LENGTH _record _n)
    message(STATUS "Kconfig: applied ${_n} value(s) from ${CONFIG_FILE}")
endfunction()

# zhln_kconfig_apply_early(<config file> <symbol>...)
#
# Applies only the named symbols from <config file> to the cache, with the
# same file-wins-once-then-cache-wins rule as zhln_kconfig_apply() but with
# its own stamp file, so the late full apply still runs afterwards.
#
# Exists for entries whose value is read before the full apply can run.
# Currently only ZHLN_JOLT_SLANG_FALLBACK: cmake/ThirdParty.cmake reads it to
# choose Jolt's shader compiler, while the full apply must wait for every
# option() declaration -- including the ones in cmake/ThirdParty.cmake
# itself. Without this, the first configure after a menuconfig run still
# sees the option() default (or a stale cache value) at that point, and a
# FATAL_ERROR there aborts before the full apply could update the cache:
# the build deadlocks, failing the same way on every re-run.
function(zhln_kconfig_apply_early CONFIG_FILE)
    if(NOT EXISTS "${CONFIG_FILE}")
        return()
    endif()

    set(_want ${ARGN})
    set(_record "")

    file(STRINGS "${CONFIG_FILE}" _lines)
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^# CONFIG_([A-Za-z0-9_]+) is not set")
            set(_name "${CMAKE_MATCH_1}")
            set(_value "OFF")
        elseif(_line MATCHES "^CONFIG_([A-Za-z0-9_]+)=(.*)$")
            set(_name "${CMAKE_MATCH_1}")
            set(_value "${CMAKE_MATCH_2}")
            if(_value STREQUAL "y")
                set(_value "ON")
            elseif(_value STREQUAL "n")
                set(_value "OFF")
            endif()
        else()
            continue()
        endif()

        list(FIND _want "${_name}" _want_idx)
        if(_want_idx EQUAL -1)
            continue()
        endif()
        list(APPEND _record "${_name}=${_value}")
    endforeach()

    if(NOT _record)
        return()
    endif()

    string(SHA256 _digest "${_record}")
    set(_stamp "${CONFIG_FILE}.early.stamp")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _previous)
        string(STRIP "${_previous}" _previous)
        if(_previous STREQUAL _digest)
            return()
        endif()
    endif()
    file(WRITE "${_stamp}" "${_digest}\n")

    foreach(_entry IN LISTS _record)
        string(REGEX REPLACE "^([A-Za-z0-9_]+)=.*$" "\\1" _name "${_entry}")
        string(REGEX REPLACE "^[A-Za-z0-9_]+=(.*)$" "\\1" _value "${_entry}")
        if(DEFINED CACHE{${_name}})
            get_property(_type CACHE ${_name} PROPERTY TYPE)
            if(NOT _type)
                set(_type BOOL)
            endif()
            set(${_name} "${_value}" CACHE ${_type} "" FORCE)
        else()
            set(${_name} "${_value}" CACHE BOOL
                "Set by the Kconfig menu (${CONFIG_FILE})")
        endif()
    endforeach()
endfunction()
