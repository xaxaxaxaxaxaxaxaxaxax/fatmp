cmake_minimum_required(VERSION 3.28)

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)

project(fmp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(FMP_GCC_MODULE_OPTIONS -fmodules -freflection)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(WARNING "fmp requires GCC 16+ C++26 reflection support; ${CMAKE_CXX_COMPILER_ID} is not expected to build it.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(${FMP_GCC_MODULE_OPTIONS})
endif()

function(fmp_target_defaults target visibility)
    target_compile_features(${target} ${visibility} cxx_std_26)
endfunction()

set(FMP_MODULE_INTERFACES
    fmp.cppm
)

set_source_files_properties(${FMP_MODULE_INTERFACES} PROPERTIES LANGUAGE CXX)

add_library(fmp)
set_target_properties(fmp PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_sources(fmp
    PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS "${PROJECT_SOURCE_DIR}"
        FILES ${FMP_MODULE_INTERFACES}
)
fmp_target_defaults(fmp PUBLIC)

function(add_fmp_example target source)
    add_executable(${target} ${source})
    set_target_properties(${target} PROPERTIES CXX_SCAN_FOR_MODULES ON)
    target_link_libraries(${target} PRIVATE fmp)
endfunction()

add_fmp_example(fmp_example example.cpp)
