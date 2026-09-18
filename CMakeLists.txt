cmake_minimum_required(VERSION 3.16)

# ชื่อโปรเจกต์
project(headshot_tool LANGUAGES CXX)

# กำหนดมาตรฐาน C++20 (หรือ C++17 ก็ได้)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# เปิดใช้งาน MOC/UIC/RCC อัตโนมัติสำหรับ Qt
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ค้นหาแพ็กเกจ Qt6 ที่จำเป็น
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# รวมไฟล์ Source ทั้งหมด
set(SOURCES
    headshot_tool.cpp
)

# สร้างไฟล์ Executable (.exe)
add_executable(headshot_tool WIN32 ${SOURCES})

# ลิงก์ Qt6 Libraries และ Windows System API ที่ใช้ในโค้ด
target_link_libraries(headshot_tool PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    User32.lib
    Psapi.lib
)

# ตั้งค่าสำหรับ MSVC compiler
if(MSVC)
    target_compile_options(headshot_tool PRIVATE /EHsc /W4)
endif()
