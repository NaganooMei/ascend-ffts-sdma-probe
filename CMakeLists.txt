cmake_minimum_required(VERSION 3.16)

project(ascend_ffts_sdma_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASCEND_ROOT "/usr/local/Ascend/ascend-toolkit/latest" CACHE PATH "Ascend toolkit root")

find_path(ASCEND_ACL_INCLUDE_DIR
    NAMES acl/acl.h
    PATHS
        "${ASCEND_ROOT}/include"
        "${ASCEND_ROOT}/aarch64-linux/include"
    NO_DEFAULT_PATH
)

find_library(ASCEND_ASCENDCL_LIBRARY
    NAMES ascendcl
    PATHS
        "${ASCEND_ROOT}/lib64"
        "${ASCEND_ROOT}/lib"
        "${ASCEND_ROOT}/aarch64-linux/lib64"
        "${ASCEND_ROOT}/aarch64-linux/lib"
    NO_DEFAULT_PATH
)

find_library(ASCEND_RUNTIME_LIBRARY
    NAMES runtime
    PATHS
        "${ASCEND_ROOT}/lib64"
        "${ASCEND_ROOT}/lib"
        "${ASCEND_ROOT}/runtime/lib64"
        "${ASCEND_ROOT}/runtime/lib"
        "${ASCEND_ROOT}/aarch64-linux/lib64"
        "${ASCEND_ROOT}/aarch64-linux/lib"
    NO_DEFAULT_PATH
)

find_path(ASCEND_FFTS_INCLUDE_DIR
    NAMES runtime/rt_ffts_plus.h rt_external_ffts.h
    PATHS
        "${ASCEND_ROOT}/include"
        "${ASCEND_ROOT}/pkg_inc"
        "${ASCEND_ROOT}/pkg_inc/runtime"
        "${ASCEND_ROOT}/aarch64-linux/pkg_inc"
        "${ASCEND_ROOT}/aarch64-linux/pkg_inc/runtime"
    NO_DEFAULT_PATH
)

foreach(required_var
        ASCEND_ACL_INCLUDE_DIR
        ASCEND_ASCENDCL_LIBRARY
        ASCEND_RUNTIME_LIBRARY
        ASCEND_FFTS_INCLUDE_DIR)
    if(NOT ${required_var})
        message(FATAL_ERROR "${required_var} was not found. Pass -DASCEND_ROOT=/path/to/ascend-toolkit/latest.")
    endif()
endforeach()

get_filename_component(ASCEND_FFTS_INCLUDE_PARENT "${ASCEND_FFTS_INCLUDE_DIR}" DIRECTORY)
set(ASCEND_FFTS_INCLUDE_DIRS "${ASCEND_FFTS_INCLUDE_DIR}")
if(ASCEND_FFTS_INCLUDE_DIR MATCHES "/runtime$" AND EXISTS "${ASCEND_FFTS_INCLUDE_PARENT}")
    list(APPEND ASCEND_FFTS_INCLUDE_DIRS "${ASCEND_FFTS_INCLUDE_PARENT}")
endif()
list(APPEND ASCEND_FFTS_INCLUDE_DIRS
    "${ASCEND_ROOT}/pkg_inc"
    "${ASCEND_ROOT}/pkg_inc/toolchain"
    "${ASCEND_ROOT}/pkg_inc/profiling"
    "${ASCEND_ROOT}/aarch64-linux/pkg_inc"
    "${ASCEND_ROOT}/aarch64-linux/pkg_inc/toolchain"
    "${ASCEND_ROOT}/aarch64-linux/pkg_inc/profiling"
)
list(REMOVE_DUPLICATES ASCEND_FFTS_INCLUDE_DIRS)

add_executable(ffts_sdma_probe
    src/ffts_sdma_probe.cpp
)

target_include_directories(ffts_sdma_probe PRIVATE
    "${ASCEND_ACL_INCLUDE_DIR}"
    ${ASCEND_FFTS_INCLUDE_DIRS}
)

target_link_libraries(ffts_sdma_probe PRIVATE
    "${ASCEND_ASCENDCL_LIBRARY}"
    "${ASCEND_RUNTIME_LIBRARY}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ffts_sdma_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()
