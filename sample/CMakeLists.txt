cmake_minimum_required(VERSION 3.18)
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    message(WARNING "CMAKE_GENERATOR must be \"Visual Studio\"")
    return()
endif()
message(STATUS "s ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "b ${CMAKE_CURRENT_BINARY_DIR}")

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
    # VS_PLATFORM_TOOLSET     "WindowsApplicationForDrivers10.0"
)

target_include_directories(custom_mft
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
    $<$<CONFIG:Debug>:${CMAKE_CURRENT_BINARY_DIR}/custom_mft.dir/Debug>
    $<$<CONFIG:Release>:${CMAKE_CURRENT_BINARY_DIR}/custom_mft.dir/Release>
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

install(FILES   custom_mft.idl
                $<$<CONFIG:Debug>:${CMAKE_CURRENT_BINARY_DIR}/custom_mft.dir/Debug/custom_mft.h>
                $<$<CONFIG:Release>:${CMAKE_CURRENT_BINARY_DIR}/custom_mft.dir/Release/custom_mft.h>
        DESTINATION include/${PROJECT_NAME}
)
install(FILES   $<TARGET_PDB_FILE:custom_mft>
        DESTINATION bin
)
install(TARGETS custom_mft
        EXPORT  ${PROJECT_NAME}-config
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
)
