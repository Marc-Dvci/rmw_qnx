cmake_minimum_required(VERSION 3.14)
project(rmw_qnx CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rcutils REQUIRED)
find_package(rmw REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_introspection_c REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)

add_library(rmw_qnx SHARED src/rmw_qnx.cpp)
target_include_directories(rmw_qnx PRIVATE core src)
target_compile_options(rmw_qnx PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(rmw_qnx
  rcutils::rcutils
  rmw::rmw
  rosidl_runtime_c::rosidl_runtime_c
  rosidl_typesupport_introspection_c::rosidl_typesupport_introspection_c
  rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp)

register_rmw_implementation("cpp:rosidl_typesupport_cpp:rosidl_typesupport_introspection_cpp")

# /dev/ros2: the graph as a resource manager.
add_executable(ros2graphd src/ros2graphd.cpp)
target_include_directories(ros2graphd PRIVATE core src)
target_link_libraries(ros2graphd
  rosidl_runtime_c::rosidl_runtime_c
  rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp)
install(TARGETS ros2graphd DESTINATION bin)

install(TARGETS rmw_qnx
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin)

ament_export_libraries(rmw_qnx)
ament_export_dependencies(rcutils rmw rosidl_runtime_c rosidl_typesupport_introspection_c rosidl_typesupport_introspection_cpp)
ament_package()
