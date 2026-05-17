# Hot-path disassembly artifacts.
#
# Listed sources get their per-TU object file dumped to ${HOTPATH_ASM_DIR}/<flat-name>.S
# after each build (POST_BUILD). Source path matching is relative to ${CMAKE_SOURCE_DIR}.
#
# Output is via `objdump -d -S -C` (source interleave via DWARF, demangled symbols).
# Intel syntax is added on x86; aarch64 uses default AT&T-ish.
#
# Usage in a target's CMakeLists.txt, after add_executable/add_library:
#     hotpath_disasm(${TARGET_NAME})

set(HOT_PATH_SOURCES
    src/bus_processor/rx_application.cpp
    src/bus_processor/process_bus_parser.cpp
    src/bus_generator/gen_application.cpp
    CACHE STRING "Sources whose disassembly is dumped under build/hotpath_asm/"
)

set(HOTPATH_ASM_DIR ${CMAKE_BINARY_DIR}/hotpath_asm)

# Objdump syntax flag — Intel on x86, omitted on aarch64.
# Stored as a CMake list (semicolon-separated) so it expands to separate
# shell args, not one quoted blob.
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|i[3-6]86")
    set(_HOTPATH_OBJDUMP_SYNTAX -M intel)
else()
    set(_HOTPATH_OBJDUMP_SYNTAX "")
endif()

set(_HOTPATH_HELPER "${CMAKE_CURRENT_LIST_DIR}/scripts/hotpath_disasm.sh")

if (NOT CMAKE_OBJDUMP)
    message(WARNING "[hotpath] CMAKE_OBJDUMP not set — disassembly artifacts disabled")
endif()

function(hotpath_disasm TARGET)
    if (NOT CMAKE_OBJDUMP)
        return()
    endif()

    get_target_property(_srcs ${TARGET} SOURCES)
    foreach(SRC ${_srcs})
        if (NOT IS_ABSOLUTE "${SRC}")
            set(SRC_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${SRC}")
        else()
            set(SRC_ABS "${SRC}")
        endif()
        get_filename_component(SRC_ABS "${SRC_ABS}" ABSOLUTE)

        file(RELATIVE_PATH SRC_REL_REPO    "${CMAKE_SOURCE_DIR}"         "${SRC_ABS}")
        file(RELATIVE_PATH SRC_REL_CALLER  "${CMAKE_CURRENT_SOURCE_DIR}" "${SRC_ABS}")

        if (NOT "${SRC_REL_REPO}" IN_LIST HOT_PATH_SOURCES)
            continue()
        endif()

        # Make/Ninja generator object-file layout (stable for our build).
        set(OBJ "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TARGET}.dir/${SRC_REL_CALLER}.o")

        # Flat output name: src/foo/bar.cpp -> src_foo_bar.cpp.S
        string(REPLACE "/" "_" ASM_NAME "${SRC_REL_REPO}")
        set(ASM "${HOTPATH_ASM_DIR}/${ASM_NAME}.S")

        # Invoke helper script with positional args — VERBATIM lets CMake
        # escape each arg properly for the shell, and the script owns the
        # mkdir + objdump + redirect locally. No quoting magic in CMake.
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND sh "${_HOTPATH_HELPER}"
                    "${HOTPATH_ASM_DIR}" "${ASM}"
                    "${CMAKE_OBJDUMP}"   "${OBJ}"
                    ${_HOTPATH_OBJDUMP_SYNTAX}
            BYPRODUCTS "${ASM}"
            COMMENT "[hotpath] disasm ${SRC_REL_REPO} -> hotpath_asm/${ASM_NAME}.S"
            VERBATIM
        )
    endforeach()
endfunction()
