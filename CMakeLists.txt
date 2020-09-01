cmake_minimum_required(VERSION 3.13)
project(media VERSION 0.0 LANGUAGES CXX)

if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS true)
endif()

find_package(Microsoft.GSL CONFIG REQUIRED)

add_library(media
    src/socket.hpp
    src/socket.cpp
)

set_target_properties(media
PROPERTIES
    CXX_STANDARD 20
    # PUBLIC_HEADER src/socket.hpp
    WINDOWS_EXPORT_ALL_SYMBOLS True # todo: remove this
)

target_include_directories(media
PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
)

target_link_libraries(media
PUBLIC
    ws2_32
    Microsoft.GSL::GSL
)

if(CMAKE_CXX_COMPILER_ID MATCHES Clang)
    if(NOT WIN32)
        message(FATAL_ERROR "the project is only for Windows OS")
    endif()
    target_compile_options(media
    PUBLIC
        /Wall
    )
elseif(MSVC)
    target_compile_options(media
    PUBLIC
        /W4
    )
endif()

install(FILES           src/socket.hpp
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
    CXX_STANDARD 20
)

find_package(Catch2 CONFIG REQUIRED)
target_link_libraries(media_test_suite
PRIVATE
    media Catch2::Catch2
)
