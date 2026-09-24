cmake_minimum_required(VERSION 3.16)
set(project_name "Onda")

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/ondacollider-version" ONDACOLLIDER_VERSION LIMIT_COUNT 1)
string(STRIP "${ONDACOLLIDER_VERSION}" ONDACOLLIDER_VERSION)
if(ONDACOLLIDER_VERSION STREQUAL "")
    message(FATAL_ERROR "Could not determine the OndaCollider version")
endif()

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

project(${project_name} VERSION "${ONDACOLLIDER_VERSION}")
message(STATUS "Building OndaCollider version: ${PROJECT_VERSION}")
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "11.0")
    message(FATAL_ERROR "Apple Silicon requires macOS 11.0 or later")
endif()
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
set(ONDA_SYSTEM_LIBS "" CACHE STRING "Additional system libs needed by the Onda static library")

set(ONDA_VERSION_FILE "${ONDA_SDK_PATH}/.onda-version")
if(NOT EXISTS "${ONDA_VERSION_FILE}")
    set(ONDA_VERSION_FILE "${CMAKE_SOURCE_DIR}/onda-version")
endif()
file(STRINGS "${ONDA_VERSION_FILE}" ONDA_BUILD_VERSION LIMIT_COUNT 1)
string(STRIP "${ONDA_BUILD_VERSION}" ONDA_BUILD_VERSION)
if(ONDA_BUILD_VERSION STREQUAL "")
    message(FATAL_ERROR "Could not determine the Onda SDK version from ${ONDA_VERSION_FILE}")
endif()
message(STATUS "Building with Onda version: ${ONDA_BUILD_VERSION}")

find_package(Onda CONFIG REQUIRED
    PATHS
        "${ONDA_SDK_PATH}/cmake"
        "${ONDA_SDK_PATH}/lib/cmake/Onda"
    NO_DEFAULT_PATH
)
if(APPLE)
    get_target_property(ONDA_STATIC_LIBRARY Onda::Static IMPORTED_LOCATION)
    execute_process(
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-macos-deployment-target.sh"
            "${CMAKE_OSX_DEPLOYMENT_TARGET}" "${ONDA_STATIC_LIBRARY}"
        RESULT_VARIABLE ONDA_SDK_TARGET_RESULT)
    if(NOT ONDA_SDK_TARGET_RESULT EQUAL 0)
        message(FATAL_ERROR
            "The Onda SDK requires a newer macOS version than "
            "${CMAKE_OSX_DEPLOYMENT_TARGET}; rebuild the SDK with "
            "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
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
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ondacollider-version" DESTINATION "Onda" RENAME "VERSION")

# ------------------------------------------------------------------
# 4. LINKING
# ------------------------------------------------------------------

function(finalize_target target_name)
    if(TARGET ${target_name})
        message(STATUS "Configuring target: ${target_name}")
        target_link_libraries(${target_name} PRIVATE Onda::Static)
        if(APPLE)
            add_custom_command(TARGET ${target_name} POST_BUILD
                COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-macos-deployment-target.sh"
                    "${CMAKE_OSX_DEPLOYMENT_TARGET}" "$<TARGET_FILE:${target_name}>"
                VERBATIM)
        endif()
        target_compile_definitions(
            ${target_name}
            PRIVATE
                ONDACOLLIDER_VERSION="${PROJECT_VERSION}"
                ONDACOLLIDER_ONDA_VERSION="${ONDA_BUILD_VERSION}"
        )
        if(ONDA_SYSTEM_LIBS)
            target_link_libraries(${target_name} PRIVATE ${ONDA_SYSTEM_LIBS})
        endif()
    else()
        message(WARNING "Target ${target_name} was NOT created.")
    endif()
endfunction()

finalize_target(Onda_scsynth)
finalize_target(Onda_supernova)

# ------------------------------------------------------------------
# 5. TESTING
# ------------------------------------------------------------------

include(CTest)
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(SCLANG_EXECUTABLE sclang)
    find_program(BASH_EXECUTABLE bash)
    find_program(SUPERNOVA_EXECUTABLE supernova)
    if(SCLANG_EXECUTABLE AND BASH_EXECUTABLE)
        add_test(
            NAME OndaIntegration
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "SCLANG_EXECUTABLE=${SCLANG_EXECUTABLE}"
                "ONDA_COLLIDER_SERVER_PROGRAM=scsynth"
                "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/run-integration.sh" "${CMAKE_BINARY_DIR}"
        )
        set_tests_properties(OndaIntegration PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

        if(TARGET Onda_supernova AND SUPERNOVA_EXECUTABLE)
            add_test(
                NAME OndaIntegrationSupernova
                COMMAND
                    "${CMAKE_COMMAND}" -E env
                    "SCLANG_EXECUTABLE=${SCLANG_EXECUTABLE}"
                    "ONDA_COLLIDER_SERVER_PROGRAM=supernova"
                    "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/run-integration.sh" "${CMAKE_BINARY_DIR}"
            )
            set_tests_properties(OndaIntegrationSupernova PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
        else()
            message(STATUS "Skipping Onda supernova integration test: target or executable unavailable")
        endif()
    else()
        message(STATUS "Skipping Onda integration test: sclang or bash was not found")
    endif()
endif()

message(STATUS "Generating plugin targets done")
