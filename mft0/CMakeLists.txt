#
# References
#   https://github.com/GoogleCloudPlatform/compute-windows-drivers/blob/master/pvpanic/pvpanic/pvpanic.vcxproj
#   https://gitlab.com/spice/win32/usbdk/-/blob/master/UsbDkInstHelper/UsbDkInstHelper.vcxproj
#
cmake_minimum_required(VERSION 3.18) # for target_precompile_headers

set(CMAKE_SUPPRESS_REGENERATION true) # no ZERO_CHECK
set(CMAKE_VS_WINRT_BY_DEFAULT true)
# set(CMAKE_VS_JUST_MY_CODE_DEBUGGING false)

add_library(MFT0 SHARED
    dllmain.cpp
    MFT0.idl MFT0Impl.h MFT0Impl.cpp MFT0clsid.h
    stdafx.h stdafxsrc.cpp 
    SampleHelpers.h CustomProperties.h MetadataInternal.h Macros.h
)

set_target_properties(MFT0
PROPERTIES
    C_STANDARD      17
    CXX_STANDARD    17
    VERSION         ${PROJECT_VERSION}
    VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION  "10.0.17764.0"
    VS_PLATFORM_TOOLSET     "WindowsApplicationForDrivers10.0" # "WindowsKernelModeDriver10.0"
    # VS_CONFIGURATION_TYPE   "Driver"
    # VS_GLOBAL_DriverTargetPlatform "Universal"
)

target_include_directories(MFT0
PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(MFT0
PRIVATE
    /W4 /WX- /bigobj /errorReport:send
)

target_link_libraries(MFT0
PRIVATE
    media # windowsapp # C++/WinRT
    mfplat # mfapi.h
)

target_link_options(MFT0
PRIVATE
    /INCREMENTAL:NO /ERRORREPORT:SEND
)

install(TARGETS MFT0
        RUNTIME DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
        LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
        ARCHIVE DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
)
