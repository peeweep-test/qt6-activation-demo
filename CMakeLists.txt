cmake_minimum_required(VERSION 3.16)
project(activation_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

add_executable(activation_demo
    main.cpp
    mainwindow.cpp
    mainwindow.h
)

target_link_libraries(activation_demo PRIVATE Qt6::Widgets)
