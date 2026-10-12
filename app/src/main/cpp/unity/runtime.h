#pragma once
#include "vendor/nlohmann/json.hpp"
#include <memory>
namespace vulkanscope::unity {
using Json = nlohmann::json;
/** @brief Target-resident IL2CPP runtime; only the agent's serialized worker
 * invokes requests. */
class Runtime {
public:
  Runtime();
  ~Runtime();
  Json request(const Json &request);
  Runtime(const Runtime &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
