#include "dashboard.h"

namespace vulkanscope {
void Dashboard::reset() {
    clear();
    modules_.push_back(createDeviceModule());
    modules_.push_back(createGpuModule());
    modules_.push_back(createRootModule());
}
void Dashboard::clear() { modules_.clear(); }
void Dashboard::draw(const ModuleContext& context) {
    for (const auto& module : modules_) module->draw(context);
}
}
