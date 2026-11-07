// chatter: a talker and a listener on /chatter (example_interfaces/msg/String), for any RMW implementation.
//
//   chatter talk [N] [PERIOD_MS]     publishes "hello <i>" N times (default 50, every 100 ms)
//   chatter listen [N] [TIMEOUT_S]   prints each message, then a CHATTER line with the count, and exits after N
//                                     messages or TIMEOUT_S seconds (default 50, 15 s)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "example_interfaces/msg/string.hpp"
#include "rclcpp/rclcpp.hpp"

using example_interfaces::msg::String;

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: chatter talk|listen [N] [PERIOD_MS|TIMEOUT_S]\n");
    return 64;
  }
  const std::string role = argv[1];
  const int n = argc > 2 ? std::atoi(argv[2]) : 50;
  rclcpp::init(argc, argv);
  if (role == "talk") {
    const int period = argc > 3 ? std::atoi(argv[3]) : 100;
    auto node = std::make_shared<rclcpp::Node>("talker");
    auto pub = node->create_publisher<String>("chatter", 10);
    rclcpp::WallRate rate{std::chrono::milliseconds(period)};
    for (int i = 1; i <= n && rclcpp::ok(); i++) {
      String m;
      m.data = "hello " + std::to_string(i);
      pub->publish(m);
      rate.sleep();
    }
    std::printf("CHATTER published=%d subscribers_matched=%zu\n", n, pub->get_subscription_count());
  } else {
    const int timeout = argc > 3 ? std::atoi(argv[3]) : 15;
    auto node = std::make_shared<rclcpp::Node>("listener");
    int got = 0;
    std::string last;
    auto sub = node->create_subscription<String>("chatter", 10, [&](const String &m) {
      got++;
      last = m.data;
      std::printf("I heard: [%s]\n", m.data.c_str());
      std::fflush(stdout);
    });
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
    rclcpp::executors::SingleThreadedExecutor exec;
    exec.add_node(node);
    while (rclcpp::ok() && got < n && std::chrono::steady_clock::now() < end)
      exec.spin_once(std::chrono::milliseconds(100));
    std::printf("CHATTER received=%d last=[%s] publishers_matched=%zu\n", got, last.c_str(), sub->get_publisher_count());
  }
  rclcpp::shutdown();
  return 0;
}
