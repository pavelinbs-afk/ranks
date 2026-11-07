cmake_minimum_required(VERSION 3.16)

# lr_core — MetaMod:Source 2 plugin (CS2) — TAB icons only
#
# Dependencies:
#   HL2SDKCS2 / HL2SDK_PATH      -> hl2sdk-cs2 root
#   MMSOURCE112 / MMS_PATH       -> metamod-source root
#
# XP / MySQL / menus / wallet live in CounterStrikeSharp LevelsRanks.
#
# Output: build/addons/lr_core/bin/linuxsteamrt64/lr_core.so

project(lr_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LR_CORE_OPTIMIZE "Enable -O3 in Release" ON)

function(lr_core_try_path OUT_VAR)
  set(${OUT_VAR} "" PARENT_SCOPE)
  foreach(candidate IN LISTS ARGN)
    if(candidate AND EXISTS "${candidate}")
      set(${OUT_VAR} "${candidate}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
endfunction()

if(NOT HL2SDK_PATH AND DEFINED ENV{HL2SDKCS2})
  set(HL2SDK_PATH "$ENV{HL2SDKCS2}")
endif()
if(NOT MMS_PATH AND DEFINED ENV{MMSOURCE112})
  set(MMS_PATH "$ENV{MMSOURCE112}")
elseif(NOT MMS_PATH AND DEFINED ENV{MMSOURCE_DEV})
  set(MMS_PATH "$ENV{MMSOURCE_DEV}")
endif()

set(_lr_roots
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/.."
  "${CMAKE_SOURCE_DIR}/../.."
  "${CMAKE_SOURCE_DIR}/../../.."
)
foreach(_root IN LISTS _lr_roots)
  list(APPEND _lr_hl2sdk_candidates
    "${_root}/deps/hl2sdk-cs2"
    "${_root}/hl2sdk-cs2"
  )
  list(APPEND _lr_mms_candidates
    "${_root}/deps/metamod-source"
    "${_root}/metamod-source"
  )
endforeach()

if(NOT HL2SDK_PATH OR NOT EXISTS "${HL2SDK_PATH}/public/tier0/dbg.h")
  lr_core_try_path(_found_hl2sdk ${_lr_hl2sdk_candidates})
  if(_found_hl2sdk)
    set(HL2SDK_PATH "${_found_hl2sdk}")
  endif()
endif()
if(NOT MMS_PATH OR NOT EXISTS "${MMS_PATH}/core/ISmmPlugin.h")
  lr_core_try_path(_found_mms ${_lr_mms_candidates})
  if(_found_mms)
    set(MMS_PATH "${_found_mms}")
  endif()
endif()

set(HL2SDK_PATH "${HL2SDK_PATH}" CACHE PATH "Path to hl2sdk-cs2")
set(MMS_PATH "${MMS_PATH}" CACHE PATH "Path to metamod-source")

if(NOT HL2SDK_PATH OR NOT EXISTS "${HL2SDK_PATH}/public/tier0/dbg.h")
  message(FATAL_ERROR "HL2SDK not found. Set HL2SDKCS2 or -DHL2SDK_PATH=...")
endif()
if(NOT MMS_PATH OR NOT EXISTS "${MMS_PATH}/core/ISmmPlugin.h")
  message(FATAL_ERROR "Metamod:Source not found. Set MMSOURCE112 or -DMMS_PATH=...")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(MMS_CORE "${MMS_PATH}/core")

add_library(lr_core_flags INTERFACE)
target_compile_definitions(lr_core_flags INTERFACE
  META_IS_SOURCE2
  PLATFORM_64BITS
  X64BITS
  _GLIBCXX_USE_CXX11_ABI=0
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_definitions(lr_core_flags INTERFACE
    COMPILER_GCC
    stricmp=strcasecmp
    _stricmp=strcasecmp
    _snprintf=snprintf
    _vsnprintf=vsnprintf
    HAVE_STDINT_H
    GNUC
  )
  target_compile_options(lr_core_flags INTERFACE
    -pipe
    -fno-strict-aliasing
    -Wall
    -Wno-uninitialized
    -Wno-parentheses
    -Wno-unused
    -Wno-switch
    -Wno-sign-compare
    -Wno-attributes
    -Wno-invalid-offsetof
    -Wno-non-virtual-dtor
    -Wno-overloaded-virtual
    -fno-threadsafe-statics
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -fno-omit-frame-pointer
    -fPIC
    -msse
  )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(lr_core_flags INTERFACE
    POSIX
    LINUX
    _LINUX
    LINUX64
    _FILE_OFFSET_BITS=64
  )
endif()
if(LR_CORE_OPTIMIZE)
  target_compile_options(lr_core_flags INTERFACE $<$<CONFIG:Release>:-O3>)
  target_compile_definitions(lr_core_flags INTERFACE $<$<CONFIG:Release>:NDEBUG>)
endif()

target_include_directories(lr_core_flags INTERFACE
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/public"
  "${MMS_CORE}"
  "${MMS_CORE}/sourcehook"
  "${MMS_PATH}/third_party/khook/include"
  "${HL2SDK_PATH}/thirdparty/protobuf-3.21.8/src"
  "${HL2SDK_PATH}/public"
  "${HL2SDK_PATH}/public/engine"
  "${HL2SDK_PATH}/public/mathlib"
  "${HL2SDK_PATH}/public/tier0"
  "${HL2SDK_PATH}/public/tier1"
  "${HL2SDK_PATH}/public/entity2"
  "${HL2SDK_PATH}/public/game/server"
  "${HL2SDK_PATH}/public/schemasystem"
  "${HL2SDK_PATH}/game/shared"
  "${HL2SDK_PATH}/game/server"
  "${HL2SDK_PATH}/common"
)

# eiface.h / inetchannel.h need generated network_connection.pb.h (SDK stubs only).
set(LR_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LR_PROTO_GEN_DIR}")

set(LR_PROTO_FILES
  "${HL2SDK_PATH}/common/valveextensions.proto"
  "${HL2SDK_PATH}/common/network_connection.proto"
  "${HL2SDK_PATH}/common/networkbasetypes.proto"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(LR_PROTOC "${HL2SDK_PATH}/devtools/bin/linux/protoc")
else()
  set(LR_PROTOC "${HL2SDK_PATH}/devtools/bin/protoc.exe")
endif()
if(NOT EXISTS "${LR_PROTOC}")
  message(FATAL_ERROR "protoc not found: ${LR_PROTOC}")
endif()

set(LR_PROTO_SRCS "")
set(LR_PROTO_HDRS "")
foreach(proto ${LR_PROTO_FILES})
  get_filename_component(proto_name "${proto}" NAME_WE)
  set(out_cc "${LR_PROTO_GEN_DIR}/${proto_name}.pb.cc")
  set(out_h "${LR_PROTO_GEN_DIR}/${proto_name}.pb.h")
  list(APPEND LR_PROTO_SRCS "${out_cc}")
  list(APPEND LR_PROTO_HDRS "${out_h}")
  add_custom_command(
    OUTPUT "${out_cc}" "${out_h}"
    COMMAND "${LR_PROTOC}"
      --cpp_out=${LR_PROTO_GEN_DIR}
      "-I${HL2SDK_PATH}/common"
      "-I${HL2SDK_PATH}/thirdparty/protobuf-3.21.8/src"
      "${proto}"
    DEPENDS "${proto}"
    COMMENT "Generating ${proto_name}.pb.cc"
    VERBATIM
  )
endforeach()
add_custom_target(lr_core_protoc DEPENDS ${LR_PROTO_SRCS} ${LR_PROTO_HDRS})

set(LR_CORE_PLUGIN_SRCS
  src/lr_core.cpp
  src/schema.cpp
  src/vtable_finder.cpp
  src/config.cpp
  src/players.cpp
  src/tab.cpp
  src/commands.cpp
)

set(LR_CORE_SDK_SRCS
  "${HL2SDK_PATH}/public/tier0/memoverride.cpp"
  "${HL2SDK_PATH}/entity2/entitysystem.cpp"
  "${HL2SDK_PATH}/entity2/entityidentity.cpp"
  "${HL2SDK_PATH}/entity2/entitykeyvalues.cpp"
  "${HL2SDK_PATH}/tier1/convar.cpp"
  "${HL2SDK_PATH}/tier1/keyvalues3.cpp"
)

add_library(lr_core SHARED
  ${LR_CORE_PLUGIN_SRCS}
  ${LR_CORE_SDK_SRCS}
  ${LR_PROTO_SRCS}
)

add_dependencies(lr_core lr_core_protoc)
target_link_libraries(lr_core PRIVATE lr_core_flags)
target_include_directories(lr_core PRIVATE "${LR_PROTO_GEN_DIR}")

set_target_properties(lr_core PROPERTIES
  PREFIX ""
  OUTPUT_NAME "lr_core"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(HL2SDK_LIB "${HL2SDK_PATH}/lib/linux64")

  set(LR_SDK_LIBS
    "${HL2SDK_LIB}/mathlib.a"
    "${HL2SDK_LIB}/interfaces.a"
    "${HL2SDK_LIB}/release/libprotobuf.a"
  )
  foreach(lib ${LR_SDK_LIBS})
    if(NOT EXISTS "${lib}")
      message(FATAL_ERROR "Missing HL2SDK library: ${lib}")
    endif()
  endforeach()

  target_link_libraries(lr_core PRIVATE
    ${LR_SDK_LIBS}
    dl
    pthread
    m
  )

  target_link_options(lr_core PRIVATE
    -static-libgcc
    -static-libstdc++
  )

  set(LR_OUT_DIR "${CMAKE_BINARY_DIR}/addons/lr_core/bin/linuxsteamrt64")
  set_target_properties(lr_core PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${LR_OUT_DIR}"
  )

  set(LR_TIER0_SO "${HL2SDK_LIB}/libtier0.so")
  if(EXISTS "${LR_TIER0_SO}")
    add_custom_command(TARGET lr_core POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LR_TIER0_SO}" "${LR_OUT_DIR}/"
      COMMENT "Copy libtier0.so next to lr_core.so"
    )
  endif()

  set(LR_PKG_METAMOD "${CMAKE_BINARY_DIR}/addons/metamod")
  set(LR_PKG_CFG "${CMAKE_BINARY_DIR}/addons/lr_core/configs")
  set(LR_PKG_TR "${CMAKE_BINARY_DIR}/addons/lr_core/translations")

  add_custom_target(lr_core_package ALL
    DEPENDS lr_core
    COMMAND ${CMAKE_COMMAND} -E rm -rf
      "${CMAKE_BINARY_DIR}/addons/lr_core/configs"
      "${CMAKE_BINARY_DIR}/addons/lr_core/translations"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LR_PKG_METAMOD}" "${LR_PKG_CFG}" "${LR_PKG_TR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/lr_core.vdf" "${LR_PKG_METAMOD}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/configs/tab.ini" "${LR_PKG_CFG}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/translations/lr_core.phrases.txt" "${LR_PKG_TR}/"
    COMMENT "Packaging addons/lr_core TAB layout"
  )

elseif(WIN32)
  set(HL2SDK_LIB "${HL2SDK_PATH}/lib/public/win64/2015")
  target_link_libraries(lr_core PRIVATE
    "${HL2SDK_LIB}/mathlib.lib"
    "${HL2SDK_LIB}/tier0.lib"
    "${HL2SDK_LIB}/interfaces.lib"
  )
  set_target_properties(lr_core PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addons/lr_core/bin/win64"
  )
else()
  message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

message(STATUS "lr_core TAB-only: HL2SDK=${HL2SDK_PATH}")
message(STATUS "lr_core TAB-only: MMS=${MMS_PATH}")
