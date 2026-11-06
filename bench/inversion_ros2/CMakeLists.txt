cmake_minimum_required(VERSION 3.14)
project(rmwqnx_inversion CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(example_interfaces REQUIRED)

add_executable(inversion src/inversion.cpp)
target_link_libraries(inversion rclcpp::rclcpp ${example_interfaces_TARGETS})
install(TARGETS inversion DESTINATION lib/${PROJECT_NAME})

ament_package()
