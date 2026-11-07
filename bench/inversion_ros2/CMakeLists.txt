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

add_executable(chatter src/chatter.cpp)
target_link_libraries(chatter rclcpp::rclcpp ${example_interfaces_TARGETS})
install(TARGETS chatter DESTINATION lib/${PROJECT_NAME})

# Guest checks of rmw_qnx's behaviour (priorities, topic objects, hostile input), called through rmw_implementation.
find_package(rcl_interfaces REQUIRED)
add_executable(rmw_checks src/rmw_checks.cpp)
target_link_libraries(rmw_checks rclcpp::rclcpp ${example_interfaces_TARGETS} ${rcl_interfaces_TARGETS})
install(TARGETS rmw_checks DESTINATION lib/${PROJECT_NAME})

ament_package()
