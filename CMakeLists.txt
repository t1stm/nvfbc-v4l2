cmake_minimum_required(VERSION 3.23)
project(nvfbc_v4l2 C)

set(CMAKE_C_STANDARD 99)

find_package(X11 REQUIRED)
find_package(OpenGL REQUIRED)

set(SOURCES NvFBC.h v4l2_wrapper.h nvfbc_v4l2.h nvfbc_v4l2.c main.c)

add_executable(nvfbc_v4l2 ${SOURCES})

target_link_libraries(nvfbc_v4l2 m X11 ${OPENGL_LIBRARIES})