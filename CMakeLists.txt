cmake_minimum_required(VERSION 3.12)

# Generate version string if it was not specified
if (NOT DEFINED SPH_VERSION)
    set(SPH_VERSION "unknown") # Set Fallback SPH version
    find_package(Git)
    if(Git_FOUND)
        execute_process(
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMAND ${GIT_EXECUTABLE} describe --tags --always
            OUTPUT_VARIABLE GIT_DESCRIBE
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE GIT_RETURN)
        # Check if the command was successful
        if(GIT_RETURN EQUAL "0")
            # Strip leading "v" from "v0.3.8-111-g04be8d2b"
            string(REGEX REPLACE "^v" "" SPH_VERSION ${GIT_DESCRIBE})
        else()
            message(WARNING "git describe failed, version detection not possible.")
        endif()
    else()
        message(WARNING "Git not found, version detection not possible.")
    endif()
    message(STATUS "Using auto-detected version \"${SPH_VERSION}\"")
endif()

project(OpenSPH LANGUAGES CXX)
# Setting project version separately: https://gitlab.kitware.com/cmake/cmake/-/issues/16716
set(CMAKE_PROJECT_VERSION ${SPH_VERSION})

option(WITH_TBB "Enable TBB parallelization" ON)
option(WITH_EIGEN "Enable additional algorithms using Eigen" OFF)
option(WITH_CHAISCRIPT "Enable scripting tools" OFF)
option(WITH_VDB "Enable conversion to OpenVDB files" OFF)
option(WITH_ALEMBIC "Enable export to Alembic files" ON)
option(WITH_HDF5 "Enable HDF5 input/output" ON)
option(BUILD_UTILS "Build auxiliary utilities" OFF)
option(USE_SINGLE_PRECISION "Build OpenSPH with single precision" OFF)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/;${CMAKE_MODULE_PATH}")

add_definitions(-DSPH_VERSION=${CMAKE_PROJECT_VERSION})

if (WIN32)
    add_definitions(-D_ENABLE_EXTENDED_ALIGNED_STORAGE -D_CRT_SECURE_NO_WARNINGS)
endif()

find_package(wxWidgets COMPONENTS core base adv propgrid aui)
if (wxWidgets_FOUND)
    include(${wxWidgets_USE_FILE})
elseif (WIN32)
    # Portable MSYS2/MinGW fallback for Windows
    if (DEFINED ENV{MINGW_PREFIX} AND EXISTS "$ENV{MINGW_PREFIX}/include/wx-3.2")
        set(_WX_ROOT "$ENV{MINGW_PREFIX}")
    elseif (EXISTS "C:/msys64/ucrt64/include/wx-3.2")
        set(_WX_ROOT "C:/msys64/ucrt64")
    endif()

    if (DEFINED _WX_ROOT)
        set(wxWidgets_FOUND TRUE)
        include_directories("${_WX_ROOT}/include/wx-3.2" "${_WX_ROOT}/lib/wx/include/msw-unicode-3.2")
        set(wxWidgets_LIBRARIES -L${_WX_ROOT}/lib -lwx_mswu_core-3.2 -lwx_baseu-3.2 -lwx_mswu_adv-3.2 -lwx_mswu_propgrid-3.2 -lwx_mswu_aui-3.2)
    endif()
endif()

if (WITH_EIGEN)
    find_package(Eigen REQUIRED)
    include_directories(${EIGEN_INCLUDE_DIRS})
    add_definitions(-DSPH_USE_EIGEN)
endif()

if (USE_SINGLE_PRECISION)
    message(STATUS "Building ${CMAKE_PROJECT_NAME} with single precision")
    add_definitions(-DSPH_SINGLE_PRECISION)
endif()

if (WITH_CHAISCRIPT)
    add_definitions(-DSPH_USE_CHAISCRIPT)
    if (WIN32)
        set(CHAISCRIPT_INCLUDE_DIR "Chaiscript_NOT_FOUND" CACHE PATH "Directory containing Chaiscript files")
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /bigobj")
        include_directories(${CHAISCRIPT_INCLUDE_DIR})
    else()
        set(CHAISCRIPT_LIBRARIES -ldl)
    endif()
endif()

if (WITH_TBB)
    find_package(TBB CONFIG QUIET)
    if (NOT TBB_FOUND)
        find_package(TBB REQUIRED tbb tbbmalloc)
        set(TBB_LIBRARIES TBB::tbb TBB::tbbmalloc)
    else()
        set(TBB_LIBRARIES TBB::tbb TBB::tbbmalloc)
    endif()
    add_definitions(-DSPH_USE_TBB)
    if(DEFINED Tbb_VERSION_MAJOR AND Tbb_VERSION_MAJOR LESS 2021)
        message(STATUS "Using classic TBB")
        add_definitions(-DTBB_SUPPRESS_DEPRECATED_MESSAGES)
    else()
        message(STATUS "Using oneTBB")
    endif()
endif()

if (WITH_VDB)
    find_package(OpenVDB REQUIRED)
    add_definitions(-DSPH_USE_VDB)
    include_directories(${OpenVDB_INCLUDE_DIRS})

    # OpenVDB requires TBB
    if (NOT WITH_TBB)
        find_package(TBB REQUIRED tbb)
        set(TBB_LIBRARIES TBB::tbb)
    endif()

    # Workaround for OpenVDB before version 8.1
    if (OpenVDB_MAJOR_VERSION LESS "9" OR (OpenVDB_MAJOR_VERSION EQUAL "8" AND OpenVDB_MINOR_VERSION LESS "1"))
        message("OpenVDB ${OpenVDB_VERSION} version requires external IlmBase IMath Half implementation.")
        find_package(IlmBase COMPONENTS Half REQUIRED) # "Half" for "libHalf"
    endif()

    # if IlmBase was required (either directly or indirectly via OpenVDB) add it to all occurances of OpenVDB
    set(OpenVDB_ALL_LIBRARIES ${OpenVDB_LIBRARIES} ${IlmBase_LIBRARIES} ${TBB_LIBRARIES})
endif()

if (WITH_ALEMBIC)
    find_package(Alembic REQUIRED)
    add_definitions(-DSPH_USE_ALEMBIC)
    include_directories(${Alembic_INCLUDE_DIRS})
endif()

if (WITH_HDF5)
    find_package(HDF5 REQUIRED COMPONENTS C)
    add_definitions(-DSPH_USE_HDF5)
    include_directories(${HDF5_INCLUDE_DIRS})
endif()

if (MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -msse4.1 -g")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DSPH_DEBUG)
endif()

add_subdirectory(core)
add_subdirectory(gui)
add_subdirectory(gui/launcherGui)
add_subdirectory(cli/launcher)
add_subdirectory(cli/info)
add_subdirectory(cli/sph2extrapresets)

if (BUILD_UTILS)
    add_subdirectory(cli/ssftotxt)
endif()
