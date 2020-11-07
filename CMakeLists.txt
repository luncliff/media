cmake_minimum_required(VERSION 3.13)
project(media VERSION 0.0 LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "the project is only for Windows Platform")
endif()
if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS true)
endif()
set(CMAKE_VS_WINRT_BY_DEFAULT true)

message(STATUS "using Windows: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

find_package(Microsoft.GSL CONFIG)
find_package(spdlog        CONFIG REQUIRED)

# see https://docs.microsoft.com/en-us/windows/win32/medfound/media-foundation-headers-and-libraries
include(CheckIncludeFileCXX)
check_include_file_cxx("mfapi.h" found_mfapi)
check_include_file_cxx("wincodecsdk.h" found_codecsdk)
check_include_file_cxx("wrl/client.h" found_wrl)
check_include_file_cxx("d3d11.h" found_direcx11)

check_include_file_cxx("winrt/Windows.Foundation.h" found_winrt)

add_library(media STATIC
    src/media.hpp
    src/media.cpp
    src/media_print.cpp
)

set_target_properties(media
PROPERTIES
    PUBLIC_HEADER src/media.hpp
    WINDOWS_EXPORT_ALL_SYMBOLS false
)

target_include_directories(media
PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(media
PUBLIC
    windowsapp
PRIVATE
    dxva2 evr shlwapi # for WinRT / COM
    mf mfplat mfplay mfreadwrite mfuuid wmcodecdspuuid # for Media Foundation
    spdlog::spdlog
)
if(Microsoft.GSL_FOUND)
    target_link_libraries(media
    PUBLIC
        Microsoft.GSL::GSL
    )
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES Clang)
    message(FATAL_ERROR "This project uses WinRT. clang-cl can't be used since <experimentatl/coroutine> is not supported anymore")
    target_compile_options(media
    PUBLIC
        /std:c++17
    PRIVATE
        /W4
    )
elseif(MSVC)
    target_compile_options(media
    PUBLIC
        /Zc:__cplusplus /std:c++17 /await
    PRIVATE
        /W4 /bigobj /errorReport:send
    )
endif()

# install(FILES           src/media.hpp
#         DESTINATION     ${CMAKE_INSTALL_PREFIX}/include/${PROJECT_NAME}
# )
install(TARGETS         media
        EXPORT          ${PROJECT_NAME}-config
        RUNTIME  DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
        LIBRARY  DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
        ARCHIVE  DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_PREFIX}/include/${PROJECT_NAME}
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
    test/main.cpp
    test/media_test.cpp
    test/direct11_test.cpp
)

set_target_properties(media_test_suite
PROPERTIES
    CXX_STANDARD 17
)

find_package(Catch2 CONFIG REQUIRED)
target_link_libraries(media_test_suite
PRIVATE
    media Catch2::Catch2 spdlog::spdlog
)

get_filename_component(asset_path ${PROJECT_SOURCE_DIR}/test ABSOLUTE)

target_compile_definitions(media_test_suite
PRIVATE
    ASSET_DIR="${asset_path}"
)

install(TARGETS  media_test_suite
        RUNTIME  DESTINATION ${CMAKE_INSTALL_PREFIX}/tools
)
