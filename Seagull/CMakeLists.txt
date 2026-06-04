cmake_minimum_required(VERSION 3.20)
project(Seagull VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_PREFIX_PATH "C:/Qt/6.11.1/msvc2022_64")

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia MultimediaWidgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_executable(Seagull 
    Seagull.cpp 
    Seagull.h
    Modules/UI/MainWindow.cpp
    Modules/UI/MainWindow.h
    Modules/UI/Library.cpp
    Modules/UI/Library.h
    Modules/UI/Downloads.cpp
    Modules/UI/Downloads.h
    Modules/UI/Search.h
    Modules/UI/Settings.h
    Modules/UI/Settings.cpp
    Modules/UI/Widgets/PlayerControls.cpp
    Modules/UI/Widgets/PlayerControls.h
    Modules/UI/Widgets/PlayerTitleBar.cpp
    Modules/UI/Widgets/PlayerTitleBar.h
    Modules/Backend/SgYtDlp.h
    Modules/Backend/SgYtDlp.cpp
    resources.qrc   # <--- Added right here at the bottom of the list!
)

if(MSVC)
    target_compile_options(Seagull PRIVATE /Zc:__cplusplus)
endif()

target_link_libraries(Seagull PRIVATE Qt6::Widgets Qt6::Multimedia Qt6::MultimediaWidgets)

add_custom_command(TARGET Seagull POST_BUILD
    COMMAND "C:/Qt/6.11.1/msvc2022_64/bin/windeployqt.exe" "$<TARGET_FILE:Seagull>"
)

# Copy the tools directory to the output build directory so the app can find yt-dlp.exe
add_custom_command(TARGET Seagull POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/tools"
    "$<TARGET_FILE_DIR:Seagull>/tools"
)