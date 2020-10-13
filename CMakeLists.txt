cmake_minimum_required(VERSION 3.13)
project(media VERSION 0.0 LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "the project is only for Windows Platform")
endif()
if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS true)
endif()
set(CMAKE_VS_WINRT_BY_DEFAULT true)

find_package(Microsoft.GSL CONFIG REQUIRED)

# see https://docs.microsoft.com/en-us/windows/win32/medfound/media-foundation-headers-and-libraries
include(CheckIncludeFileCXX)
check_include_file_cxx("codecapi.h"         found_codecapi)
check_include_file_cxx("mfapi.h"            found_mfapi)
check_include_file_cxx("wmcontainer.h"      found_wmcontainer)
check_include_file_cxx("shlwapi.h"          found_shlwapi)
check_include_file_cxx("wrl/client.h"       found_wrl)

add_library(media STATIC
    src/media.hpp
    src/media.cpp
)

set_target_properties(media
PROPERTIES
    PUBLIC_HEADER src/media.hpp
    WINDOWS_EXPORT_ALL_SYMBOLS False
)

target_include_directories(media
PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(media
PUBLIC
    dxva2 evr shlwapi
    mf mfplat mfplay mfreadwrite 
    mfuuid wmcodecdspuuid
    Microsoft.GSL::GSL
)

if(CMAKE_CXX_COMPILER_ID MATCHES Clang)
    target_compile_options(media
    PUBLIC
        /W4 /std:c++17
    )
elseif(MSVC)
    target_compile_options(media
    PUBLIC
        /W4 /std:c++17 /Zc:__cplusplus
    PRIVATE
        /await
    )
endif()

install(FILES           src/media.hpp
        DESTINATION     ${CMAKE_INSTALL_PREFIX}/include/${PROJECT_NAME}
)
install(TARGETS         media
        EXPORT          ${PROJECT_NAME}-config
        RUNTIME  DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
        LIBRARY  DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
)
install(EXPORT      ${PROJECT_NAME}-config
        DESTINATION ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME}
)

include(CMakePackageConfigHelpers)
set(VERSION_FILE_PATH   ${CMAKE_BINARY_DIR}/cmake/${PROJECT_NAME}-config-version.cmake)
write_basic_package_version_file(${VERSION_FILE_PATH}
    VERSION             ${PROJECT_VERSION}
    COMPATIBILITY       SameMajorVersion
)
install(FILES           ${VERSION_FILE_PATH} 
        DESTINATION     ${CMAKE_INSTALL_PREFIX}/share/${PROJECT_NAME}
)

if(NOT BUILD_TESTING)
    return()
endif()

add_executable(media_test_suite
    test/media_test.cpp
)

set_target_properties(media_test_suite
PROPERTIES
    CXX_STANDARD 17
)

find_package(Catch2 CONFIG REQUIRED)
target_link_libraries(media_test_suite
PRIVATE
    media Catch2::Catch2
)

get_filename_component(asset_path ${PROJECT_SOURCE_DIR}/test ABSOLUTE)

target_compile_definitions(media_test_suite
PRIVATE
    ASSET_DIR="${asset_path}"
)
