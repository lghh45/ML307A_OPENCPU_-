cmake_minimum_required(VERSION 3.10)
project(SMS CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(sms_debian sms_debian.cpp)
target_link_options(sms_debian PRIVATE
    -static-libgcc
    -static-libstdc++
    "$<$<CONFIG:Release>:-Wl,--gc-sections;-s>")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/epoll_framework/CMakeLists.txt")
  add_subdirectory(epoll_framework)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server_src/debian_receiver.cpp")
  add_executable(receiver server_src/debian_receiver.cpp ${VK_AUTH_SOURCES})
  target_link_libraries(receiver PRIVATE epoll_framework)
  target_link_options(receiver PRIVATE
      -static-libgcc
      -static-libstdc++
      "$<$<CONFIG:Release>:-Wl,--gc-sections;-s>")
endif()
