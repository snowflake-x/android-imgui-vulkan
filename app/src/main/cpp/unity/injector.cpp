#include "memory/scan.h"
#include "unity/wire.h"
#include "vendor/nlohmann/json.hpp"
#include <atomic>
#include <frida-core.h>
#include <fstream>
#include <jni.h>
#include <sstream>
#include <thread>

namespace {
std::string readFile(const std::string &path) {
  std::ifstream f(path);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}
std::string run(int pid, uint64_t start, const std::string &request,
                const std::string &agent) {
  using namespace vulkanscope;
  using namespace vulkanscope::unity;
  if (getuid() != 0 || pid <= 0 || !start)
    throw std::runtime_error("Root and selected target required");
  const auto base = "/proc/" + std::to_string(pid) + "/";
  auto validate = [&] {
    if (memory::StartTicks(readFile(base + "stat")) != start)
      throw std::runtime_error("Target exited or PID reused");
  };
  validate();
  auto status = readFile(base + "status");
  auto at = status.find("Uid:");
  unsigned uid = 0;
  if (at == std::string::npos)
    throw std::runtime_error("Cannot verify target UID");
  std::istringstream(status.substr(at + 4)) >> uid;
  if (uid % 100000 < 10000 || uid % 100000 >= 99000)
    throw std::runtime_error("Select a regular application process");
  auto q = nlohmann::json::parse(request);
  auto op = q.at("op").get<std::string>();
  auto name =
      "vulkanscope.unity." + std::to_string(pid) + "." + std::to_string(start);
  auto addr = endpoint(name);
  auto connectSocket = [&]() {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 &&
        connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0)
      return fd;
    if (fd >= 0)
      close(fd);
    return -1;
  };
  int fd = connectSocket();
  if (fd < 0) {
    if (op != "connect")
      throw std::runtime_error(
          "Runtime not connected or session expired; reconnect");
    if (agent.empty() || agent.size() > (64u << 20))
      throw std::runtime_error("Invalid agent library");
    frida_init();
    auto injector = frida_injector_new();
    auto cancel = g_cancellable_new();
    GError *error = nullptr;
    std::atomic<bool> done{false};
    std::thread timer([&] {
      for (int i = 0; i < 100 && !done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!done)
        g_cancellable_cancel(cancel);
    });
    GBytes *blob = g_bytes_new_static(agent.data(), agent.size());
    auto id = frida_injector_inject_library_blob_sync(
        injector, pid, blob, "vulkanscope_unity_main", name.c_str(), cancel,
        &error);
    done = true;
    timer.join();
    g_bytes_unref(blob);
    g_object_unref(cancel);
    std::string message = error ? error->message : "";
    if (error)
      g_error_free(error);
    if (id)
      frida_injector_demonitor_sync(injector, id, nullptr, nullptr);
    frida_injector_close_sync(injector, nullptr, nullptr);
    g_object_unref(injector);
    if (!id)
      throw std::runtime_error("Root attach failed: " + message);
    for (int i = 0; i < 40 && fd < 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      fd = connectSocket();
    }
    if (fd < 0)
      throw std::runtime_error(
          "Agent did not open its endpoint; check ABI and SELinux diagnostics");
  }
  Socket socket(fd);
  timeout(fd);
  ucred peer{};
  socklen_t len = sizeof(peer);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) || peer.pid != pid ||
      peer.uid != uid)
    throw std::runtime_error("Runtime peer identity mismatch");
  validate();
  transmit(fd, request);
  auto result = receive(fd);
  validate();
  return result + "\nMEMSCAN_DONE\n";
}
}
/** @brief Disposable root helper; all attach and wire operations run off the
 * Android UI thread. */
extern "C" JNIEXPORT jbyteArray JNICALL
Java_dev_snowflake_vulkan_MemoryProbeBootstrap_nativeInspector(
    JNIEnv *e, jclass, jint pid, jlong start, jbyteArray request,
    jbyteArray agent) {
  try {
    auto copy = [&](jbyteArray a) {
      auto n = e->GetArrayLength(a);
      std::string s(n, 0);
      e->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte *>(s.data()));
      return s;
    };
    auto result = run(pid, start, copy(request), copy(agent));
    auto out = e->NewByteArray(result.size());
    e->SetByteArrayRegion(out, 0, result.size(),
                          reinterpret_cast<const jbyte *>(result.data()));
    return out;
  } catch (const std::exception &error) {
    e->ThrowNew(e->FindClass("java/lang/IllegalStateException"), error.what());
    return nullptr;
  }
}
