cmake_minimum_required(VERSION 3.11)

project(hicolorconvert)

find_package(Qt6 COMPONENTS Core Concurrent REQUIRED)

add_executable(hicolorconvert main.cpp)
target_link_libraries(hicolorconvert PRIVATE Qt6::Core Qt6::Concurrent)

# 设置 C++ 标准
set_property(TARGET hicolorconvert PROPERTY CXX_STANDARD 17)
set_property(TARGET hicolorconvert PROPERTY CXX_STANDARD_REQUIRED ON)

install(TARGETS hicolorconvert DESTINATION libexec/deepin-desktop-theme)
