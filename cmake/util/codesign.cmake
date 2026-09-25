find_program(CODESIGN_COMMAND codesign REQUIRED)

function(codesign_sign_files files signature keychain)
    if (NOT signature)
        message(FATAL_ERROR "codesign failed: signature is required")
    endif()

    set(args
        --force
        --verbose
        --timestamp
        --options runtime
        --sign "${signature}"
    )

    if(keychain)
        list(APPEND args --keychain "${keychain}")
    endif()

    foreach(file IN LISTS files)
        set(cmd ${CODESIGN_COMMAND} ${args} "${file}")

        list(JOIN cmd " " cmd_str)
        message(STATUS "${cmd_str}")

        execute_process(
            COMMAND ${cmd}
            RESULT_VARIABLE result
            ERROR_VARIABLE error
        )
        if(NOT result EQUAL 0)
            string(REPLACE "\n" "\n  " error "  ${error}")
            message(FATAL_ERROR "codesign failed:\n${error}")
        endif()
    endforeach()
endfunction()

# Ad-hoc signs every piece of code under `root` (no signing identity).
#
# On Apple Silicon all code must carry a valid signature. The linker ad-hoc
# signs what it produces, but deploying Qt (install_name_tool, strip)
# invalidates those signatures, and the kernel kills the app with
# "Code Signature Invalid" as soon as it maps such a file (e.g. a Qt plugin).
function(codesign_adhoc_sign_tree root)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0009 NEW) # do not follow symlinks (Versions/Current)
    file(GLOB_RECURSE entries LIST_DIRECTORIES true "${root}/*")
    cmake_policy(POP)

    set(machos "")
    set(bundles "")
    foreach(entry IN LISTS entries)
        if(IS_SYMLINK "${entry}")
            continue()
        endif()
        if(IS_DIRECTORY "${entry}")
            if(entry MATCHES [[\.(framework|app|appex|bundle)$]])
                list(APPEND bundles "${entry}")
            endif()
            continue()
        endif()
        file(READ "${entry}" magic LIMIT 4 HEX)
        # Mach-O (32/64-bit, both byte orders) and universal binaries.
        if(magic MATCHES "^(feedface|feedfacf|cefaedfe|cffaedfe|cafebabe|bebafeca|cafebabf|bfbafeca)$")
            list(APPEND machos "${entry}")
        endif()
    endforeach()

    # The main executable of a bundle is signed with the bundle: codesign on
    # its path signs the whole bundle, which fails while nested code is not
    # signed yet.
    foreach(bundle IN LISTS bundles)
        get_filename_component(bundle_name "${bundle}" NAME_WE)
        set(executable "${bundle_name}")
        set(plist "${bundle}/Contents/Info.plist")
        if(NOT EXISTS "${plist}")
            set(plist "${bundle}/Resources/Info.plist")
        endif()
        if(EXISTS "${plist}")
            file(READ "${plist}" plist_content)
            if(plist_content MATCHES "<key>CFBundleExecutable</key>[ \t\r\n]*<string>([^<]+)</string>")
                set(executable "${CMAKE_MATCH_1}")
            endif()
        endif()
        string(REGEX REPLACE [[([][+.*^$?|()\{}])]] [[\\\1]] bundle_re "${bundle}")
        string(REGEX REPLACE [[([][+.*^$?|()\{}])]] [[\\\1]] executable_re "${executable}")
        list(FILTER machos EXCLUDE REGEX "^${bundle_re}/(Contents/MacOS|Versions/[^/]+)/${executable_re}$")
    endforeach()

    # Nested bundles before the bundles that contain them.
    set(keyed "")
    foreach(bundle IN LISTS bundles)
        string(REGEX MATCHALL "/" slashes "${bundle}")
        list(LENGTH slashes depth)
        # Always four digits, deeper paths sort first.
        math(EXPR key "9999 - ${depth}")
        list(APPEND keyed "${key}|${bundle}")
    endforeach()
    list(SORT keyed)
    set(bundles "")
    foreach(item IN LISTS keyed)
        string(REGEX REPLACE "^[0-9]+\\|" "" item "${item}")
        list(APPEND bundles "${item}")
    endforeach()

    list(LENGTH machos machos_count)
    list(LENGTH bundles bundles_count)
    message(STATUS "Ad-hoc signing ${machos_count} binaries and ${bundles_count} bundles in ${root}")

    foreach(file IN LISTS machos bundles)
        execute_process(
            COMMAND ${CODESIGN_COMMAND} --force --sign - "${file}"
            RESULT_VARIABLE result
            ERROR_VARIABLE error
        )
        if(NOT result EQUAL 0)
            string(REPLACE "\n" "\n  " error "  ${error}")
            message(FATAL_ERROR "codesign failed for ${file}:\n${error}")
        endif()
    endforeach()

    # Fail the packaging instead of shipping code the kernel will reject.
    foreach(file IN LISTS machos bundles)
        execute_process(
            COMMAND ${CODESIGN_COMMAND} --verify "${file}"
            RESULT_VARIABLE result
            ERROR_VARIABLE error
        )
        if(NOT result EQUAL 0)
            string(REPLACE "\n" "\n  " error "  ${error}")
            message(FATAL_ERROR "codesign verification failed for ${file}:\n${error}")
        endif()
    endforeach()
endfunction()
