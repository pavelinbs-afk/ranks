cmake_minimum_required(VERSION 3.16)

# lr_core — MetaMod:Source 2 plugin (CS2)
#
# Dependencies (paths via -D or environment):
#   HL2SDKCS2 / HL2SDK_PATH      -> hl2sdk-cs2 root
#   MMSOURCE112 / MMS_PATH       -> metamod-source root
#   MARIADB_PREFIX               -> static mariadb-connector-c (default: ../deps/mariadb via build_cmake.sh)
#
# Quick start (WSL):
#   sudo apt install -y build-essential cmake git libssl-dev zlib1g-dev
#   bash build_cmake.sh
#
# Output: build/addons/lr_core/bin/linuxsteamrt64/lr_core.so

project(lr_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LR_CORE_OPTIMIZE "Enable -O3 in Release" ON)

# ---------------------------------------------------------------------------
# Paths (env -> -D cache -> auto-search like AMBuildScript)
# ---------------------------------------------------------------------------

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

# Common layouts: repo/deps, sibling deps, docker /work/deps
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
  list(APPEND _lr_mariadb_candidates
    "${_root}/deps/mariadb"
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

if(NOT MARIADB_PREFIX OR NOT EXISTS "${MARIADB_PREFIX}/lib/mariadb/libmariadbclient.a")
  if(DEFINED ENV{MARIADB_PREFIX})
    set(MARIADB_PREFIX "$ENV{MARIADB_PREFIX}")
  else()
    lr_core_try_path(_found_mdb ${_lr_mariadb_candidates})
    if(_found_mdb)
      set(MARIADB_PREFIX "${_found_mdb}")
    endif()
  endif()
endif()

set(HL2SDK_PATH "${HL2SDK_PATH}" CACHE PATH "Path to hl2sdk-cs2")
set(MMS_PATH "${MMS_PATH}" CACHE PATH "Path to metamod-source")
set(MARIADB_PREFIX "${MARIADB_PREFIX}" CACHE PATH "mariadb-connector-c install prefix")

if(NOT HL2SDK_PATH OR NOT EXISTS "${HL2SDK_PATH}/public/tier0/dbg.h")
  message(FATAL_ERROR
    "HL2SDK not found.\n"
    "  export HL2SDKCS2=/path/to/hl2sdk-cs2\n"
    "  or: cmake -DHL2SDK_PATH=/path/to/hl2sdk-cs2 ...\n"
    "Clone (branch cs2):\n"
    "  git clone --branch cs2 --depth 1 https://github.com/alliedmodders/hl2sdk.git hl2sdk-cs2")
endif()
if(NOT MMS_PATH OR NOT EXISTS "${MMS_PATH}/core/ISmmPlugin.h")
  message(FATAL_ERROR
    "Metamod:Source not found.\n"
    "  export MMSOURCE112=/path/to/metamod-source\n"
    "  or: cmake -DMMS_PATH=/path/to/metamod-source ...\n"
    "Clone:\n"
    "  git clone --recursive --depth 1 https://github.com/alliedmodders/metamod-source.git")
endif()
if(NOT MARIADB_PREFIX OR NOT EXISTS "${MARIADB_PREFIX}/lib/mariadb/libmariadbclient.a")
  message(FATAL_ERROR
    "MariaDB connector not found (${MARIADB_PREFIX}/lib/mariadb/libmariadbclient.a).\n"
    "Run: bash build_cmake.sh   (builds deps/mariadb automatically)\n"
    "Or set -DMARIADB_PREFIX=... to a static mariadb-connector-c install.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(MMS_CORE "${MMS_PATH}/core")

# ---------------------------------------------------------------------------
# Toolchain / flags (mirrors AMBuildScript)
# ---------------------------------------------------------------------------

add_library(lr_core_flags INTERFACE)

target_compile_definitions(lr_core_flags INTERFACE
  META_IS_SOURCE2
  SOURCE_ENGINE=25
  SE_CS2=25
  GAME_DLL
  RAD_TELEMETRY_DISABLED
  stricmp=strcasecmp
  _stricmp=strcasecmp
  _snprintf=snprintf
  _vsnprintf=vsnprintf
  HAVE_STDINT_H
  GNUC
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
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
  )
  target_compile_options(lr_core_flags INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(lr_core_flags INTERFACE
    LINUX
    _LINUX
    POSIX
    _FILE_OFFSET_BITS=64
    X64BITS
    PLATFORM_64BITS
    COMPILER_GCC
    _GLIBCXX_USE_CXX11_ABI=0
  )
  target_compile_options(lr_core_flags INTERFACE -fPIC -msse)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND LR_CORE_OPTIMIZE)
    target_compile_options(lr_core_flags INTERFACE -O3)
    target_compile_definitions(lr_core_flags INTERFACE NDEBUG)
  endif()
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(lr_core_flags INTERFACE -g3)
    target_compile_definitions(lr_core_flags INTERFACE DEBUG _DEBUG)
  endif()
endif()

target_include_directories(lr_core_flags INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/public"
  "${MMS_CORE}"
  "${MMS_CORE}/sourcehook"
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
  "${MARIADB_PREFIX}/include/mariadb"
)

# ---------------------------------------------------------------------------
# Protobuf (usermessages.pb.h for chat)
# ---------------------------------------------------------------------------

set(LR_PROTO_FILES
  "${HL2SDK_PATH}/common/valveextensions.proto"
  "${HL2SDK_PATH}/common/network_connection.proto"
  "${HL2SDK_PATH}/common/networkbasetypes.proto"
  "${HL2SDK_PATH}/game/shared/usermessages.proto"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(LR_PROTOC "${HL2SDK_PATH}/devtools/bin/linux/protoc")
else()
  set(LR_PROTOC "${HL2SDK_PATH}/devtools/bin/protoc.exe")
endif()

if(NOT EXISTS "${LR_PROTOC}")
  message(FATAL_ERROR "protoc not found: ${LR_PROTOC}")
endif()

set(LR_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LR_PROTO_GEN_DIR}")

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
      "-I${HL2SDK_PATH}/game/shared"
      "-I${HL2SDK_PATH}/thirdparty/protobuf-3.21.8/src"
      "${proto}"
    DEPENDS "${proto}"
    COMMENT "Generating ${proto_name}.pb.cc"
    VERBATIM
  )
endforeach()

add_custom_target(lr_core_protoc DEPENDS ${LR_PROTO_SRCS} ${LR_PROTO_HDRS})

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------

set(LR_CORE_PLUGIN_SRCS
  src/lr_core.cpp
  src/schema.cpp
  src/vtable_finder.cpp
  src/chat.cpp
  src/config.cpp
  src/players.cpp
  src/events.cpp
  src/db.cpp
  src/tab.cpp
  src/commands.cpp
  src/menu.cpp
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

# ---------------------------------------------------------------------------
# Link (Linux x86_64)
# ---------------------------------------------------------------------------

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

  set(LR_MARIADB_LIB "${MARIADB_PREFIX}/lib/mariadb/libmariadbclient.a")
  if(NOT EXISTS "${LR_MARIADB_LIB}")
    message(FATAL_ERROR "Missing MariaDB static lib: ${LR_MARIADB_LIB}")
  endif()

  # Static OpenSSL only — CS2 server images often lack libssl.so.3.
  set(LR_SSL_LIB "")
  set(LR_CRYPTO_LIB "")
  set(LR_Z_LIB "")
  foreach(prefix /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib)
    if(NOT LR_SSL_LIB AND EXISTS "${prefix}/libssl.a")
      set(LR_SSL_LIB "${prefix}/libssl.a")
    endif()
    if(NOT LR_CRYPTO_LIB AND EXISTS "${prefix}/libcrypto.a")
      set(LR_CRYPTO_LIB "${prefix}/libcrypto.a")
    endif()
    if(NOT LR_Z_LIB)
      if(EXISTS "${prefix}/libz.a")
        set(LR_Z_LIB "${prefix}/libz.a")
      elseif(EXISTS "${prefix}/libz.so")
        set(LR_Z_LIB "${prefix}/libz.so")
      elseif(EXISTS "${prefix}/libz.so.1")
        set(LR_Z_LIB "${prefix}/libz.so.1")
      endif()
    endif()
  endforeach()
  if(NOT LR_Z_LIB)
    set(_lr_mariadb_zlib "${CMAKE_SOURCE_DIR}/../deps/mariadb-connector-c/build-static/external/zlib/libzlib.a")
    if(EXISTS "${_lr_mariadb_zlib}")
      set(LR_Z_LIB "${_lr_mariadb_zlib}")
    endif()
  endif()
  if(NOT LR_SSL_LIB OR NOT LR_CRYPTO_LIB)
    message(FATAL_ERROR "OpenSSL static libraries not found (libssl.a, libcrypto.a). Install: sudo apt install libssl-dev")
  endif()
  if(NOT LR_Z_LIB)
    message(FATAL_ERROR "zlib not found (install: sudo apt install zlib1g-dev)")
  endif()

  target_link_libraries(lr_core PRIVATE
    ${LR_SDK_LIBS}
    ${LR_MARIADB_LIB}
    -Wl,-Bstatic
    ${LR_SSL_LIB}
    ${LR_CRYPTO_LIB}
    -Wl,-Bdynamic
    ${LR_Z_LIB}
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

  # Optional: assemble deploy tree (like AMBuild package/)
  set(LR_PKG_METAMOD "${CMAKE_BINARY_DIR}/addons/metamod")
  set(LR_PKG_CFG "${CMAKE_BINARY_DIR}/addons/lr_core/configs")
  set(LR_PKG_TR "${CMAKE_BINARY_DIR}/addons/lr_core/translations")

  add_custom_target(lr_core_package ALL
    DEPENDS lr_core
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LR_PKG_METAMOD}" "${LR_PKG_CFG}" "${LR_PKG_TR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/lr_core.vdf" "${LR_PKG_METAMOD}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/configs/settings.ini" "${LR_PKG_CFG}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/configs/tab.ini" "${LR_PKG_CFG}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/configs/database.ini.example" "${LR_PKG_CFG}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/translations/lr_core.phrases.txt" "${LR_PKG_TR}/"
    COMMENT "Packaging addons/lr_core layout"
  )

elseif(WIN32)
  set(HL2SDK_LIB "${HL2SDK_PATH}/lib/public/win64/2015")
  target_link_libraries(lr_core PRIVATE
    "${HL2SDK_LIB}/libprotobuf.lib"
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

message(STATUS "lr_core: HL2SDK=${HL2SDK_PATH}")
message(STATUS "lr_core: MMS=${MMS_PATH}")
message(STATUS "lr_core: MariaDB=${MARIADB_PREFIX}")
