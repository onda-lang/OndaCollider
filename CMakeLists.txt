cmake_minimum_required(VERSION 3.16)
set(project_name "Onda")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------
# 1. SETUP SUPERCOLLIDER
# ------------------------------------------------------------------

if(NOT DEFINED SC_PATH OR "${SC_PATH}" STREQUAL "")
    message(FATAL_ERROR "SC_PATH is not set. Run cmake with -DSC_PATH=...")
endif()

file(TO_CMAKE_PATH "${SC_PATH}" SC_PATH)

list(APPEND CMAKE_MODULE_PATH "${SC_PATH}/cmake_modules" "${SC_PATH}/common")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake_modules")

include(SuperColliderServerPlugin RESULT_VARIABLE server_plugin_found)
if(NOT server_plugin_found)
    message(FATAL_ERROR "Could not find server plugin module. Check SC_PATH.")
endif()

include(SuperColliderCompilerConfig)
include(InstallRequiredSystemLibraries)

set(SC_PATH "${SC_PATH}" CACHE PATH "Path to SC Source" FORCE)
include("${SC_PATH}/SCVersion.txt")
message(STATUS "Building for SuperCollider version: ${SC_VERSION}")

project(${project_name})
sc_do_initial_compiler_config()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Match the statically linked Windows Onda SDK build (/MT, /MTd).
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ------------------------------------------------------------------
# 2. CONFIGURATION OPTIONS
# ------------------------------------------------------------------

option(NATIVE "Optimize for native architecture" OFF)
option(SCSYNTH "Build plugins for scsynth" ON)
option(SUPERNOVA "Build plugins for supernova" ON)
option(NOVA_SIMD "Build plugins with nova-simd support." ON)

if(NOVA_SIMD)
    add_definitions(-DNOVA_SIMD)
    include_directories("${SC_PATH}/external_libraries/nova-simd")
endif()

if(DEFINED ONDA_LLVM_PATH AND NOT DEFINED ONDA_SDK_PATH)
    set(ONDA_SDK_PATH "${ONDA_LLVM_PATH}" CACHE PATH "Path to extracted Onda SDK" FORCE)
endif()

set(ONDA_SDK_PATH "${ONDA_SDK_PATH}" CACHE PATH "Path to extracted Onda SDK")
set(ONDA_INCLUDE_DIR "${ONDA_SDK_PATH}/include" CACHE PATH "Path to Onda headers")
set(ONDA_LIBRARY_DIR "${ONDA_SDK_PATH}/lib" CACHE PATH "Path to Onda static libraries")
set(ONDA_BUILD_LIBRARY_DIR "${ONDA_SDK_PATH}/target/release" CACHE PATH "Path to locally built Onda libraries")
set(ONDA_SYSTEM_LIBS "" CACHE STRING "Additional system libs needed by the Onda static library")

if(NOT EXISTS "${ONDA_INCLUDE_DIR}/onda.h")
    message(FATAL_ERROR "onda.h not found. Set ONDA_INCLUDE_DIR or ONDA_SDK_PATH.")
endif()

if(NOT DEFINED ONDA_LIBRARY OR "${ONDA_LIBRARY}" STREQUAL "")
    if(WIN32)
        set(_onda_library_candidates
            "${ONDA_LIBRARY_DIR}/onda.lib"
            "${ONDA_LIBRARY_DIR}/onda.dll.lib"
            "${ONDA_BUILD_LIBRARY_DIR}/onda.lib"
            "${ONDA_BUILD_LIBRARY_DIR}/onda.dll.lib"
        )
        file(GLOB _onda_library_fallbacks "${ONDA_LIBRARY_DIR}/*.lib" "${ONDA_BUILD_LIBRARY_DIR}/*.lib")
    else()
        set(_onda_library_candidates
            "${ONDA_LIBRARY_DIR}/libonda.a"
            "${ONDA_BUILD_LIBRARY_DIR}/libonda.a"
        )
        file(GLOB _onda_library_fallbacks "${ONDA_LIBRARY_DIR}/*.a" "${ONDA_BUILD_LIBRARY_DIR}/*.a")
    endif()

    set(_onda_resolved_library "")
    foreach(candidate IN LISTS _onda_library_candidates)
        if(EXISTS "${candidate}")
            set(_onda_resolved_library "${candidate}")
            break()
        endif()
    endforeach()

    if("${_onda_resolved_library}" STREQUAL "" AND _onda_library_fallbacks)
        list(SORT _onda_library_fallbacks)
        list(GET _onda_library_fallbacks 0 _onda_resolved_library)
    endif()

    set(ONDA_LIBRARY "${_onda_resolved_library}" CACHE FILEPATH "Path to prebuilt Onda static library" FORCE)
endif()

if(NOT EXISTS "${ONDA_LIBRARY}")
    message(FATAL_ERROR "Onda static library not found. Set ONDA_LIBRARY or ONDA_SDK_PATH.")
endif()

# ------------------------------------------------------------------
# 3. DEFINE PLUGIN TARGET
# ------------------------------------------------------------------

set(Onda_cpp_files
    plugins/Onda/Onda.cpp
    plugins/Onda/Onda.hpp
)

set(Onda_sc_files
    plugins/Onda/Onda.sc
)

set(Onda_schelp_files
    plugins/Onda/Onda.schelp
    plugins/Onda/OndaDef.schelp
)

sc_add_server_plugin(
    "Onda"
    "Onda"
    "${Onda_cpp_files}"
    "${Onda_sc_files}"
    "${Onda_schelp_files}"
)

# ------------------------------------------------------------------
# 4. LINKING
# ------------------------------------------------------------------

function(finalize_target target_name)
    if(TARGET ${target_name})
        message(STATUS "Configuring target: ${target_name}")
        target_include_directories(${target_name} PRIVATE "${ONDA_INCLUDE_DIR}")
        target_link_libraries(${target_name} PRIVATE "${ONDA_LIBRARY}")
        target_link_libraries(${target_name} PRIVATE ${CMAKE_DL_LIBS})
        if(WIN32)
            target_link_libraries(${target_name} PRIVATE Ws2_32 Userenv Ntdll)
        endif()
        if(ONDA_SYSTEM_LIBS)
            target_link_libraries(${target_name} PRIVATE ${ONDA_SYSTEM_LIBS})
        endif()
    else()
        message(WARNING "Target ${target_name} was NOT created.")
    endif()
endfunction()

finalize_target(Onda_scsynth)
finalize_target(Onda_supernova)

message(STATUS "Generating plugin targets done")
