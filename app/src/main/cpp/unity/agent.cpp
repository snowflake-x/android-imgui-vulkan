#include "unity/runtime.h"
#include "unity/wire.h"
#include <atomic>
#include <chrono>
#include <frida-gum.h>
#include <mutex>
#include <poll.h>
#include <thread>

namespace {
std::atomic<bool> running{false};
std::once_flag initialized;
/** @brief A resident native image keeps delegate callbacks valid after
 * cancellation and disconnect. */
void serve(std::string name) {
  using namespace vulkanscope::unity;
  try {
    std::call_once(initialized, [] { gum_init_embedded(); });
    Socket listener(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    auto addr = endpoint(name);
    if (listener.fd < 0 ||
        bind(listener.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ||
        listen(listener.fd, 4))
      throw std::runtime_error("Cannot bind runtime endpoint");
    std::unique_ptr<Runtime> runtime;
    const auto session = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    bool detached = false;
    uint64_t sequence = 0;
    while (!detached) {
      pollfd event{listener.fd, POLLIN, 0};
      if (poll(&event, 1, 60000) <= 0)
        break;
      Socket peer(accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC));
      if (peer.fd < 0)
        continue;
      ucred creds{};
      socklen_t len = sizeof(creds);
      if (getsockopt(peer.fd, SOL_SOCKET, SO_PEERCRED, &creds, &len) ||
          creds.uid != 0)
        continue;
      timeout(peer.fd);
      Json result;
      try {
        auto q = Json::parse(receive(peer.fd));
        pollfd alive{peer.fd, POLLIN | POLLRDHUP, 0};
        poll(&alive, 1, 0);
        if (alive.revents & (POLLERR | POLLHUP | POLLRDHUP))
          continue;
        auto op = q.at("op").get<std::string>();
        if (op != "connect" && q.value("session", std::string()) != session)
          throw std::runtime_error(
              "Runtime session expired; reconnect and select fresh objects");
        if (op == "detach") {
          runtime.reset();
          detached = true;
          result = {{"detached", true}};
        } else {
          if (!runtime)
            runtime = std::make_unique<Runtime>();
          result = runtime->request(q);
        }
      } catch (const std::exception &e) {
        result = {{"error", e.what()}};
      }
      result["session"] = session;
      result["sequence"] = ++sequence;
      try {
        auto output =
            result.dump(-1, ' ', false, Json::error_handler_t::replace);
        if (output.size() > (480u << 10))
          output = Json{{"error",
                         "Runtime result exceeds 480 KiB; narrow the query"},
                        {"session", session}}
                       .dump();
        transmit(peer.fd, output);
      } catch (...) {
      }
    }
  } catch (...) {
  }
  running = false;
}
}
/** @brief Frida entrypoint owns one cancellable-by-disconnect session; no hooks
 * run at library load. */
extern "C" __attribute__((visibility("default"))) void
vulkanscope_unity_main(const char *data, int *unloadPolicy, void *) {
  *unloadPolicy = 1;
  bool expected = false;
  if (!running.compare_exchange_strong(expected, true))
    return;
  try {
    std::thread(serve, std::string(data)).detach();
  } catch (...) {
    running = false;
  }
}
