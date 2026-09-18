cmake_minimum_required(VERSION 3.16)
project(headshot_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(headshot_tool
    headshot_tool.cpp
)

target_link_libraries(headshot_tool PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    User32
)

# ซ่อนหน้าต่าง Console เมื่อรันบน Windows (เป็น App แบบ GUI)
set_target_properties(headshot_tool PROPERTIES
    WIN32_EXECUTABLE TRUE
)
