cmake_minimum_required(VERSION 3.18)
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    message(WARNING "CMAKE_GENERATOR must be \"Visual Studio\"")
    return()
endif()

add_library(custom_mft SHARED
    dllmain.cpp
    custom_mft.idl
)

set_target_properties(custom_mft
PROPERTIES
    C_STANDARD      17
    CXX_STANDARD    17
    VERSION         "0.0"
    VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION  "10.0.18362.0"
    VS_PLATFORM_TOOLSET     "WindowsApplicationForDrivers10.0"
)

target_include_directories(custom_mft
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_options(custom_mft
PRIVATE
    /W4 /WX- /bigobj /errorReport:send
)

target_link_libraries(custom_mft
PRIVATE
    media windowsapp spdlog::spdlog
)

target_link_options(custom_mft
PRIVATE
    /INCREMENTAL:NO /ERRORREPORT:SEND
)
