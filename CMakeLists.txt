cmake_minimum_required(VERSION 3.16)
project(media VERSION 0.0 LANGUAGES CXX)
if(NOT WIN32)
    message(FATAL_ERROR "the project is only for Windows Platform")
endif()
if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS true)
endif()
set(CMAKE_SUPPRESS_REGENERATION true) # no ZERO_CHECK
set(CMAKE_VS_WINRT_BY_DEFAULT true)
set(CMAKE_C_STANDARD 17)

message(STATUS "using Windows: ${CMAKE_SYSTEM_VERSION}")

add_compile_options(
    /wd4819 # codepage warnings
)

find_package(Microsoft.GSL CONFIG)
find_package(spdlog        CONFIG REQUIRED)

# see https://docs.microsoft.com/en-us/windows/win32/medfound/media-foundation-headers-and-libraries
include(CheckIncludeFileCXX)
check_include_file_cxx("mfapi.h" found_mfapi)
check_include_file_cxx("wincodecsdk.h" found_codecsdk)
check_include_file_cxx("d3d11.h" found_d3d11)

# see https://github.com/microsoft/wil/wiki/RAII-resource-wrappers
find_path(WIL_INCLUDE_DIRS "wil/com.h")
message(STATUS "using WIL: ${WIL_INCLUDE_DIRS}")

add_library(media STATIC
    src/media.hpp
    src/media.cpp
    src/media_impl.cpp
    src/media_print.cpp
)

set_target_properties(media
PROPERTIES
    PUBLIC_HEADER   src/media.hpp
    WINDOWS_EXPORT_ALL_SYMBOLS false
)

target_precompile_headers(media
PUBLIC
    src/media.hpp
)

target_include_directories(media
PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(media
PUBLIC
    mf mfplat mfplay mfreadwrite mfuuid wmcodecdspuuid # for Media Foundation SDK
    windowsapp shlwapi comctl32 # for WinRT / COM
PRIVATE
    dxva2 evr d3d9 d3d11 dxguid dxgi # for DXVA
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
elseif(MSVC)
    target_compile_options(media
    PUBLIC
        /Zc:__cplusplus /std:c++17 /await
    PRIVATE
        /W4 /bigobj /errorReport:send
    )
    target_link_options(media
    PRIVATE
        /ERRORREPORT:SEND
    )
endif()

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
# include(CPack)

if(BUILD_DRIVER)
    # add_subdirectory(mft0)
endif()

if(NOT BUILD_TESTING)
    return()
endif()
# see 'docs/cmake-integration.md' in https://github.com/catchorg/catch2
find_package(Catch2 CONFIG REQUIRED)
include(Catch)

add_executable(media_test_suite
    test/main.cpp
    test/webcam_test.cpp
    test/sink_test.cpp
    test/dxva_test.cpp
    test/transform_test.cpp
    test/source_test.cpp
)
if(TEST_WINRT)
    target_sources(media_test_suite
    PRIVATE
        test/winrt_test.cpp
    )
endif()

target_precompile_headers(media_test_suite REUSE_FROM media)

# see https://github.com/microsoft/cppwinrt/releases
set_target_properties(media_test_suite
PROPERTIES
    VS_CPPWINRT true
    VS_PACKAGE_REFERENCES "Microsoft.Windows.CppWinRT_2.0.201217.4"
)

target_compile_options(media_test_suite
PRIVATE
    /W4
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
    CATCH_CONFIG_WCHAR
)

catch_discover_tests(media_test_suite)

install(TARGETS  media_test_suite
        RUNTIME  DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)
