# caOS FREE - build of the examples and of your own applications (workspace/app_N)
#
#   ./bootstrap          configure for the MCU in cpu.cmake
#   ./do                 build everything      ./do 1 33      examples 1 and 33
#   ./do -a 1            build workspace/app_1
#
# Self-contained: everything this file uses is inside this package.
cmake_minimum_required(VERSION 3.20)
project(caOS_FREE C ASM)

include(${CMAKE_SOURCE_DIR}/cpu.cmake)

# --- MCU -> family/board ----------------------------------------------------
set(_BOARDS F446 f4xx/f446  F767 f7xx/f767  H563 h5xx/h563  H753 h7xx/h753  U575 u5xx/u575)
list(FIND _BOARDS "${CURR_MCU}" _i)
if(_i LESS 0)
    message(FATAL_ERROR "CURR_MCU=${CURR_MCU}: caOS FREE supports F446, F767, H563, H753, U575")
endif()
math(EXPR _i "${_i} + 1")
list(GET _BOARDS ${_i} BOARD_PATH)
set(BOARD_DIR ${CMAKE_SOURCE_DIR}/${BOARD_PATH})
string(TOLOWER "${CURR_MCU}" MCU_LC)

# MCU_DEFINES, MCU_OPTIONS, MCU_LINKER_SCRIPT, CMSIS_DEVICE - the same the
# libraries were built with: application and library must share the ABI.
include(${BOARD_DIR}/mcu.cmake)

# --- the precompiled libraries ------------------------------------------------
foreach(_lib caOS caOS_${CURR_MCU} fatfs)
    add_library(${_lib} STATIC IMPORTED)
    set_target_properties(${_lib} PROPERTIES IMPORTED_LOCATION ${BOARD_DIR}/lib${_lib}.a)
endforeach()

set(CAOS_INCLUDES
    ${CMAKE_SOURCE_DIR}/include                     # caOS headers + os_config.h (FREE)
    ${BOARD_DIR}                                    # objmap.h, config.h
    ${CMAKE_SOURCE_DIR}/external/cmsis/Core/Include
    ${CMAKE_SOURCE_DIR}/external/cmsis/Device/${CMSIS_DEVICE}/Include
    ${CMAKE_SOURCE_DIR}/external/fatfs/source
    ${CMAKE_SOURCE_DIR}/external/caos_fatfs
    ${CMAKE_SOURCE_DIR}/external/thread-metric-benchmark/include
)
set(CAOS_C_OPTIONS ${MCU_OPTIONS} -std=gnu11 -mthumb -Wall --specs=nano.specs)
set(CAOS_LINK_OPTIONS ${MCU_OPTIONS} --specs=nano.specs -T${MCU_LINKER_SCRIPT}
    -Wl,--gc-sections -static)

set(ELF_DIR ${CMAKE_SOURCE_DIR}/elfs/${CURR_MCU})

# BUILD_VER for the applications (the app_2 bootloader prints it), shaped like
# the libraries' own: "<package commit> - <build time> - FREE".
set(_commit unknown)
if(EXISTS ${CMAKE_SOURCE_DIR}/VERSION)
    file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION _line REGEX "^commit ")
    if(_line)
        string(REGEX REPLACE "^commit +" "" _commit "${_line}")
    endif()
endif()
string(TIMESTAMP _now "%Y-%m-%d %H:%M:%S")
set(CAOS_BUILD_VER "${_commit} - ${_now} - FREE")

# One firmware: every .c of a directory, linked against caOS.
function(caos_firmware NAME DIR)
    file(GLOB _srcs ${DIR}/*.c)
    if(NOT _srcs)
        return()
    endif()
    set(_t ${MCU_LC}_${NAME})
    add_executable(${_t} ${_srcs})
    target_compile_definitions(${_t} PRIVATE ${MCU_DEFINES} BUILD_VER="${CAOS_BUILD_VER}")
    target_include_directories(${_t} PRIVATE ${DIR} ${CAOS_INCLUDES})
    target_compile_options(${_t} PRIVATE ${CAOS_C_OPTIONS})
    target_link_options(${_t} PRIVATE ${CAOS_LINK_OPTIONS} -Wl,-Map=${_t}.map)
    target_link_libraries(${_t} -Wl,--start-group caOS_${CURR_MCU} caOS fatfs -lc -lm -Wl,--end-group)
    set_target_properties(${_t} PROPERTIES SUFFIX ".elf"
                          RUNTIME_OUTPUT_DIRECTORY ${ELF_DIR})
    add_custom_command(TARGET ${_t} POST_BUILD COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${_t}>)
endfunction()

# Examples: example_N, except those marked as not working with the FREE
# libraries (fail_exNN_info.md says why).
file(GLOB _ex LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/examples/example_*)
foreach(_d IN LISTS _ex)
    get_filename_component(_n ${_d} NAME)
    string(REGEX MATCH "^example_([0-9]+)$" _m "${_n}")
    if(_m AND NOT EXISTS ${_d}/fail_ex${CMAKE_MATCH_1}_info.md)
        caos_firmware(example_${CMAKE_MATCH_1} ${_d})
    endif()
endforeach()

# The bootloader map (workspace/app_2/doc), read from include/boot_image.h:
# the values the libraries were built with.
file(STRINGS ${CMAKE_SOURCE_DIR}/include/boot_image.h _bm
     REGEX "^#define CA_BOOT_(LOADER_LEN|SLOT_LEN|SLOT_B_BASE) ")
foreach(_l IN LISTS _bm)
    string(REGEX MATCH "CA_BOOT_([A-Z_]+) +(0x[0-9A-Fa-f]+)" _x "${_l}")
    set(BOOT_${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
endforeach()
if(NOT BOOT_LOADER_LEN OR NOT BOOT_SLOT_LEN OR NOT BOOT_SLOT_B_BASE)
    message(FATAL_ERROR "boot map not found in include/boot_image.h")
endif()
math(EXPR BOOT_SLOT_A_ORG "0x08000000 + ${BOOT_LOADER_LEN} + 0x400" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR BOOT_SLOT_B_ORG "${BOOT_SLOT_B_BASE} + 0x400" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR BOOT_IMAGE_LEN  "${BOOT_SLOT_LEN} - 0x400" OUTPUT_FORMAT HEXADECIMAL)

# Your applications: workspace/app_N, one firmware each.
file(GLOB _wp LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/workspace/app_*)
foreach(_d IN LISTS _wp)
    get_filename_component(_n ${_d} NAME)
    string(REGEX MATCH "^app_([0-9]+)$" _m "${_n}")
    if(_m AND IS_DIRECTORY ${_d})
        set(_app app_${CMAKE_MATCH_1})
        caos_firmware(${_app} ${_d})
        # The bootloader itself (boot.cmake): linked inside its area, so a
        # bootloader grown past CA_BOOT_LOADER_LEN fails here instead of
        # overlapping slot A on the board.
        if(EXISTS ${_d}/boot.cmake AND CURR_MCU MATCHES "^(H563|H753)$")
            target_link_options(${MCU_LC}_${_app} PRIVATE
                -Wl,--defsym=__caos_flash_length=${BOOT_LOADER_LEN})
        endif()
        # An image for the app_2 bootloader slots (workspace/app_2/doc): the
        # app's image.cmake sets CAOS_IMAGE_SLOTS (A and/or B), and each slot
        # gets a target of its own, linked inside that slot after the 1 KB
        # image header: <mcu>_app_N_slotA / _slotB. Only the boards the
        # bootloader runs on.
        if(EXISTS ${_d}/image.cmake AND CURR_MCU MATCHES "^(H563|H753)$")
            unset(CAOS_IMAGE_SLOTS)
            include(${_d}/image.cmake)
            foreach(_slot IN LISTS CAOS_IMAGE_SLOTS)
                if(_slot STREQUAL "A")
                    set(_org ${BOOT_SLOT_A_ORG})    # bank 1, after the bootloader
                elseif(_slot STREQUAL "B")
                    set(_org ${BOOT_SLOT_B_ORG})    # bank 2
                else()
                    message(FATAL_ERROR "${_app}/image.cmake: slot '${_slot}' is not A or B")
                endif()
                caos_firmware(${_app}_slot${_slot} ${_d})
                target_link_options(${MCU_LC}_${_app}_slot${_slot} PRIVATE
                    -Wl,--defsym=__caos_flash_origin=${_org}
                    -Wl,--defsym=__caos_flash_length=${BOOT_IMAGE_LEN})   # slot - 1 KB header
            endforeach()
        endif()
    endif()
endforeach()
