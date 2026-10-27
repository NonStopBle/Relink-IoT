# Builds the examples against the precompiled library in ../lib:
#   cmake -S . -B build && cmake --build build      (-DRELINK_STATIC=ON for librelink.a)
cmake_minimum_required(VERSION 3.14)
project(relink_examples CXX)
set(CMAKE_CXX_STANDARD 17)
set(relink_DIR "${CMAKE_CURRENT_LIST_DIR}/../lib/cmake/relink")
find_package(relink REQUIRED)
set(CMAKE_BUILD_RPATH "${CMAKE_CURRENT_LIST_DIR}/../lib")

foreach(ex basic listen camera bytes)
  add_executable(relink_${ex} ${ex}.cpp)
  target_link_libraries(relink_${ex} relink::relink)
endforeach()

# OpenGL camera viewer: only built when GLUT and libjpeg are installed.
find_package(OpenGL)
find_package(GLUT)
find_package(JPEG)
if(OPENGL_FOUND AND GLUT_FOUND AND JPEG_FOUND)
  add_executable(relink_camera_gl camera_gl.cpp)
  target_link_libraries(relink_camera_gl relink::relink OpenGL::GL GLUT::GLUT JPEG::JPEG)
else()
  message(STATUS "relink_camera_gl skipped (needs OpenGL, freeglut, libjpeg)")
endif()
