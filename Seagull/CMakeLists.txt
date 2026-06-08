cmake_minimum_required(VERSION 3.20)
project(Seagull VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64")

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# 1. Define SDK Paths
set(VLC_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
include_directories("${VLC_SDK_DIR}/include")

# 2. Find VLC Libs
find_library(LIBVLC_LIB NAMES libvlc vlc PATHS "${VLC_SDK_DIR}/lib" NO_DEFAULT_PATH)
find_library(LIBVLCCORE_LIB NAMES libvlccore vlccore PATHS "${VLC_SDK_DIR}/lib" NO_DEFAULT_PATH)

add_executable(Seagull 
    Seagull.cpp 
    Seagull.h
    Modules/UI/MainWindow.cpp
    Modules/UI/MainWindow.h
    Modules/UI/Library.cpp
    Modules/UI/Library.h
    Modules/UI/Queue.cpp
    Modules/UI/Queue.h
    Modules/UI/Theme.cpp
    Modules/UI/Theme.h
    Modules/UI/Search.h
    Modules/UI/Settings.h
    Modules/UI/Settings.cpp
    Modules/UI/Widgets/PlayerControls.cpp
    Modules/UI/Widgets/PlayerControls.h
    Modules/UI/Widgets/PlayerTitleBar.cpp
    Modules/UI/Widgets/PlayerTitleBar.h
    Modules/Backend/SgYtDlp.h
    Modules/Backend/SgYtDlp.cpp
    resources.qrc
)


if(MSVC)
    target_compile_options(Seagull PRIVATE /Zc:__cplusplus)
    # 4. Tell debugger where DLLs/Plugins are (No file copying needed)
    set_target_properties(Seagull PROPERTIES 
        VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Qt\\6.11.1\\msvc2022_64\\bin;${VLC_SDK_DIR}\\lib;%PATH%\nVLC_PLUGIN_PATH=${VLC_SDK_DIR}\\plugins"
    )
endif()

# 5. Link libraries
target_link_libraries(Seagull PRIVATE 
    Qt6::Widgets 
    Qt6::Multimedia 
    Qt6::MultimediaWidgets
    ${LIBVLC_LIB} 
    ${LIBVLCCORE_LIB}
)

add_custom_command(TARGET Seagull POST_BUILD
    COMMAND "C:/Qt/6.11.1/msvc2022_64/bin/windeployqt.exe" "$<TARGET_FILE:Seagull>"
)

add_custom_command(TARGET Seagull POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/Tools"
    "$<TARGET_FILE_DIR:Seagull>/Tools"
)
# --- Copy VLC Runtime Files to the Output Directory ---

# Copy the core DLLs
add_custom_command(TARGET Seagull POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${VLC_SDK_DIR}/lib/libvlc.dll"
    "$<TARGET_FILE_DIR:Seagull>/libvlc.dll"
)

add_custom_command(TARGET Seagull POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${VLC_SDK_DIR}/lib/libvlccore.dll"
    "$<TARGET_FILE_DIR:Seagull>/libvlccore.dll"
)

# Copy the plugins folder
add_custom_command(TARGET Seagull POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${VLC_SDK_DIR}/plugins"
    "$<TARGET_FILE_DIR:Seagull>/plugins"
)