#include "monitor/sampler.h"
#include <chrono>
#include <stdexcept>
#include <thread>
using namespace vulkanscope::monitor;
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}
int main() {
    Config c; c.root = true; c.interval = 250;
    std::atomic<int> app{1}, graphicsCalls{0}, frameCalls{0};
    std::atomic<bool> releaseGraphics{false};
    ParallelSampler sampler({}, false, false,
        [&](SamplePart part, const Config &, const Values &target, const std::atomic<bool> &cancel) {
            SamplePacket out;
            if (part == SamplePart::Foreground) {
                out.values = {{"foreground", "game" + std::to_string(app.load())}, {"foreground_user", "0"}, {"root_status", "granted"}};
            } else if (part == SamplePart::Graphics) {
                ++graphicsCalls;
                const auto old = target.at("foreground");
                while (!releaseGraphics && !cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                out.values["graphics_api"] = old;
            } else if (part == SamplePart::Presentation) {
                ++frameCalls;
                out.values["frame_owner"] = target.at("foreground");
                out.values["fps"] = "60";
                out.frames.fps = 60;
                out.frames.intervals = {16.67};
            }
            return out;
        });
    auto until = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            auto start = std::chrono::steady_clock::now();
            auto s = sampler.tick(c, 1, MonotonicNs());
            require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100), "tick must not wait for slow worker");
            if (predicate(s)) return s;
            require(std::chrono::steady_clock::now() < deadline, "worker result did not arrive");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    until([&](const ParallelSample &s) { return s.values.count("frame_owner") && s.values.at("frame_owner") == "game1" && graphicsCalls == 1; });
    for (int i = 0; i < 20; ++i) sampler.tick(c, 1, MonotonicNs());
    require(graphicsCalls == 1, "no queued or overlapping slow jobs");
    app = 2;
    auto switched = until([](const ParallelSample &s) { return s.values.at("foreground") == "game2"; });
    require(!switched.values.count("frame_owner") || switched.values.at("frame_owner") == "game2", "old app frame result leaked");
    require(switched.values.at("graphics_api") != "game1", "old app API leaked");
    until([](const ParallelSample &s) { return s.values.count("frame_owner") && s.values.at("frame_owner") == "game2"; });
    releaseGraphics = true;
    until([](const ParallelSample &s) {
        require(s.values.at("graphics_api") != "game1", "late cancelled result leaked");
        return s.values.at("graphics_api") == "game2";
    });
    c.interval = 5000;
    auto reset = sampler.tick(c, 2, MonotonicNs());
    require(reset.targetChanged && reset.values.at("refresh_interval_ms") == "5000", "configuration change must invalidate prior jobs");
    require(!reset.values.count("frame_owner"), "old configuration cache survived");
    sampler.stop();

    std::atomic<bool> started{false};
    ParallelSampler cancelled({}, false, false,
        [&](SamplePart, const Config &, const Values &, const std::atomic<bool> &cancel) {
            started = true;
            while (!cancel) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return SamplePacket{};
        });
    cancelled.tick(c, 1, MonotonicNs());
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto stop = std::chrono::steady_clock::now();
    cancelled.stop();
    require(std::chrono::steady_clock::now() - stop < std::chrono::milliseconds(500), "all workers must be cancelled before any join");
}
