cmake_minimum_required(VERSION 3.20)
# SemVer single source of truth. PROJECT_VERSION must stay numeric MAJOR.MINOR.PATCH;
# the pre-release tag (e.g. -beta, or empty for a final release) is appended separately.
project(Seagull VERSION 0.15.0)
set(SEAGULL_VERSION_SUFFIX "-beta")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64")

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets Svg Network)

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
    Modules/UI/VideoPlayer.cpp
    Modules/UI/VideoPlayer.h
    Modules/UI/FileExplorer.cpp
    Modules/UI/FileExplorer.h
    Modules/UI/MediaLibrary.cpp
    Modules/UI/MediaLibrary.h
    Modules/UI/Queue.cpp
    Modules/UI/Queue.h
    Modules/UI/Theme.cpp
    Modules/UI/Theme.h
    Modules/UI/Search.h
    Modules/UI/Search.cpp
    Modules/UI/Settings.h
    Modules/UI/Settings.cpp
    Modules/UI/DownloadManager.h
    Modules/UI/DownloadManager.cpp
    Modules/UI/EQ.h
    Modules/UI/EQ.cpp
    Modules/UI/SetupDialog.h
    Modules/UI/SetupDialog.cpp
    Modules/UI/Widgets/PlayerControls.cpp
    Modules/UI/Widgets/PlayerControls.h
    Modules/UI/Widgets/ClickSlider.h
    Modules/UI/Widgets/MarqueeLabel.cpp
    Modules/UI/Widgets/MarqueeLabel.h
    Modules/UI/Widgets/PlayerTitleBar.cpp
    Modules/UI/Widgets/PlayerTitleBar.h
    Modules/UI/Widgets/VideoCard.cpp
    Modules/UI/Widgets/VideoCard.h
    Modules/UI/Widgets/DownloadRow.cpp
    Modules/UI/Widgets/DownloadRow.h
    Modules/UI/Widgets/UpdateDialog.cpp
    Modules/UI/Widgets/UpdateDialog.h
    Modules/UI/Widgets/FlowLayout.cpp
    Modules/UI/Widgets/FlowLayout.h
    Modules/UI/Widgets/SpellCheckLineEdit.cpp
    Modules/UI/Widgets/SpellCheckLineEdit.h
    Modules/UI/Widgets/Visualizer.cpp
    Modules/UI/Widgets/Visualizer.h
    Modules/UI/Widgets/CircleGlyphButton.cpp
    Modules/UI/Widgets/CircleGlyphButton.h
    Modules/Backend/SgYtDlp.h
    Modules/Backend/SgYtDlp.cpp
    Modules/Backend/SgMetaCache.h
    Modules/Backend/SgMetaCache.cpp
    Modules/Backend/SgHlsProxy.h
    Modules/Backend/SgHlsProxy.cpp
    Modules/Backend/SgRecorder.h
    Modules/Backend/SgRecorder.cpp
    Modules/Backend/SgSearch.h
    Modules/Backend/SgSearch.cpp
    Modules/Backend/SgSpellCheck.h
    Modules/Backend/SgSpellCheck.cpp
    Modules/Backend/SgFormat.h
    Modules/Backend/SgFormat.cpp
    Modules/Backend/SgOptions.h
    Modules/Backend/SgOptions.cpp
    Modules/Backend/SgPaths.h
    Modules/Backend/SgPaths.cpp
    Modules/Backend/SgFavorites.h
    Modules/Backend/SgFavorites.cpp
    Modules/Backend/SgWatchHistory.h
    Modules/Backend/SgWatchHistory.cpp
    Modules/Backend/SgDownloadHistory.h
    Modules/Backend/SgDownloadHistory.cpp
    Modules/Backend/SgThumbnailer.h
    Modules/Backend/SgThumbnailer.cpp
    Modules/Backend/SgUpdater.h
    Modules/Backend/SgUpdater.cpp
    Modules/Backend/PlaybackEngine.h
    Modules/Backend/PlaybackEngine.cpp
    Modules/Backend/SgDynamics.h
    Modules/Backend/SgDynamics.cpp
    Modules/Backend/SgEq.h
    Modules/Backend/SgEq.cpp
    Modules/Backend/SgMediaControls.h
    Modules/Backend/SgMediaControls.cpp
    Modules/Backend/SgAppUpdate.h
    Modules/Backend/SgAppUpdate.cpp
    Modules/Backend/SgLog.h
    Modules/Backend/SgLog.cpp
    resources.qrc
    app.rc
)


# Hand the SemVer string to the code (e.g. "0.9.0-beta") so the app can show it.
target_compile_definitions(Seagull PRIVATE
    SEAGULL_VERSION="${PROJECT_VERSION}${SEAGULL_VERSION_SUFFIX}"
)

if(MSVC)
    target_compile_options(Seagull PRIVATE /Zc:__cplusplus)
    # 4. Tell debugger where DLLs/Plugins are (No file copying needed)
    set_target_properties(Seagull PROPERTIES
        VS_DEBUGGER_ENVIRONMENT "PATH=C:\\Qt\\6.11.1\\msvc2022_64\\bin;${VLC_SDK_DIR}\\lib;%PATH%\nVLC_PLUGIN_PATH=${VLC_SDK_DIR}\\plugins"
    )
    # Release: build as a Windows GUI app so no console window pops up behind the
    # UI (libvlc logs to it otherwise). Keeping /ENTRY:mainCRTStartup means our
    # plain main() stays the entry point — no WinMain needed. Debug keeps the
    # console subsystem so VLC/diagnostic output is still visible while debugging.
    target_link_options(Seagull PRIVATE
        $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
        $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
    )
endif()

# 5. Link libraries
target_link_libraries(Seagull PRIVATE 
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::MultimediaWidgets
    Qt6::Svg
    Qt6::Network
    ${LIBVLC_LIB}
    ${LIBVLCCORE_LIB}
    ole32           # CoCreateInstance / COM for SgSpellCheck (Spell Checking API)
    RuntimeObject   # WinRT activation (combase) for SgMediaControls (SMTC)
    shell32         # SetCurrentProcessExplicitAppUserModelID (SMTC app identity)
    dwmapi          # DwmSetWindowAttribute (disable maximize/restore transition anim)
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