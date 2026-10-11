#include "monitor/sampler.h"
#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace vulkanscope::monitor {
namespace {
constexpr std::size_t Count = 8;
constexpr const char *Names[] = {"foreground", "frames", "graphics", "memory", "gpu_time", "engine", "hardware", "platform"};
struct Job { Config config; Values target; std::uint64_t epoch = 0; };
struct Result { SamplePacket packet; std::uint64_t epoch = 0; std::int64_t started = 0, finished = 0; };
/** @brief 单一工作线程独占 Collector 与跟踪资源；不会并发操作同一通道的历史。 */
class Lane {
  public:
    Lane(SamplePart part, std::function<PlatformSample()> platform, bool gpuBlocked, bool graphicsBlocked,
         ParallelSampler::Task task)
        : collector_(cancel_, {}, gpuBlocked, graphicsBlocked), gpuBlocked_(gpuBlocked), graphicsBlocked_(graphicsBlocked) {
        worker_ = std::thread([this, part, platform = std::move(platform), task = std::move(task)] {
            for (;;) {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    wake_.wait(lock, [this] { return stopping_ || pending_; });
                    if (stopping_) return;
                    job = std::move(job_); pending_ = false;
                }
                Result result; result.epoch = job.epoch; result.started = MonotonicNs();
                try {
                    if (lastEpoch_ != job.epoch) { collector_.reset(); lastEpoch_ = job.epoch; }
                    if (task) result.packet = task(part, job.config, job.target, cancel_);
                    else if (part == SamplePart::Platform) {
                        result.packet.platform = platform ? platform() : PlatformSample{};
                        result.packet.values = result.packet.platform.values;
                    } else {
                        result.packet.values = collector_.samplePart(job.config, part, job.target);
                        result.packet.frames = collector_.frameSample();
                    }
                } catch (const std::exception &error) {
                    result.packet.values["sampling_error_" + std::string(Names[static_cast<unsigned>(part)])] = error.what();
                    collector_.reset();
                }
                result.finished = MonotonicNs();
                gpuBlocked_ = collector_.gpuTimingBlocked();
                graphicsBlocked_ = collector_.graphicsTracingBlocked();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!cancel_) ready_ = std::move(result);
                    busy_ = false;
                }
            }
        });
    }
    ~Lane() { cancel(); join(); }
    void invalidate() { std::lock_guard<std::mutex> lock(mutex_); cancel_ = true; ready_.reset(); }
    void cancel() { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; cancel_ = true; wake_.notify_all(); }
    void join() { if (worker_.joinable()) worker_.join(); }
    bool submit(Job job) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (busy_ || stopping_) return false;
        job_ = std::move(job); cancel_ = false; busy_ = pending_ = true; wake_.notify_one(); return true;
    }
    std::optional<Result> take() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto value = std::move(ready_); ready_.reset(); return value;
    }
    bool gpuBlocked() const { return gpuBlocked_; }
    bool graphicsBlocked() const { return graphicsBlocked_; }
  private:
    std::atomic<bool> cancel_{false};
    Collector collector_;
    std::atomic<bool> gpuBlocked_, graphicsBlocked_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool busy_ = false, pending_ = false, stopping_ = false;
    Job job_;
    std::optional<Result> ready_;
    std::uint64_t lastEpoch_ = 0;
    std::thread worker_;
};
std::string get(const Values &v, const char *key) {
    const auto it = v.find(key); return it == v.end() ? std::string{} : it->second;
}
}
struct ParallelSampler::State {
    std::array<std::unique_ptr<Lane>, Count> lanes;
    std::array<std::optional<Result>, Count> cache;
    std::uint64_t revision = 0, epoch = 1;
    bool initialized = false;
    std::string identity;
    Values target;
};
ParallelSampler::ParallelSampler(std::function<PlatformSample()> platform, bool gpuBlocked, bool graphicsBlocked, Task task)
    : state_(std::make_unique<State>()) {
    for (std::size_t i = 0; i < Count; ++i)
        state_->lanes[i] = std::make_unique<Lane>(static_cast<SamplePart>(i), platform, gpuBlocked, graphicsBlocked, task);
}
ParallelSampler::~ParallelSampler() { stop(); }
void ParallelSampler::stop() {
    for (auto &lane : state_->lanes) if (lane) lane->cancel();
    for (auto &lane : state_->lanes) if (lane) lane->join();
}
bool ParallelSampler::gpuTimingBlocked() const { return state_->lanes[4]->gpuBlocked(); }
bool ParallelSampler::graphicsTracingBlocked() const { return state_->lanes[2]->graphicsBlocked(); }
ParallelSample ParallelSampler::tick(const Config &c, std::uint64_t revision, std::int64_t now) {
    auto &s = *state_;
    ParallelSample output;
    if (!s.initialized || revision != s.revision) {
        s.initialized = true; s.revision = revision; ++s.epoch;
        for (auto &lane : s.lanes) lane->invalidate();
        s.cache = {}; s.target.clear(); s.identity.clear(); output.targetChanged = true;
    }
    if (auto result = s.lanes[0]->take(); result && result->epoch == revision) {
        s.cache[0] = std::move(result);
        s.target = s.cache[0]->packet.values;
    }
    const auto focusAge = s.cache[0] ? now - s.cache[0]->started : 0;
    if (s.cache[0] && focusAge > std::max(1000, c.interval * 2) * 1000000LL) {
        s.target = {{"foreground", "N/A (foreground query overdue)"}};
    }
    const auto identity = get(s.target, "root_status") + ":" + get(s.target, "foreground_user") + ":" + get(s.target, "foreground");
    if (identity != s.identity) {
        s.identity = identity; ++s.epoch; output.targetChanged = true;
        for (std::size_t i = 1; i < 6; ++i) { s.lanes[i]->invalidate(); s.cache[i].reset(); }
    }
    const bool awake = !s.cache[7] || s.cache[7]->packet.platform.interactive;
    if (!awake) {
        for (std::size_t i = 0; i < 7; ++i) { s.lanes[i]->invalidate(); s.cache[i].reset(); }
        s.target.clear(); s.identity.clear(); output.targetChanged = true;
    }
    const bool hasTarget = awake && s.target.count("foreground_user") != 0;
    output.values = s.target;
    if (s.cache[0]) {
        output.values["sample_age_ms_foreground"] = std::to_string(std::max<std::int64_t>(0, focusAge) / 1000000);
        output.values["sample_duration_ms_foreground"] = std::to_string((s.cache[0]->finished - s.cache[0]->started) / 1000000);
    }
    for (const auto *id : {"fps", "frame_time", "frame_p95", "fps_source", "app_memory", "gpu_time", "gpu_memory", "engine_name"})
        output.values.try_emplace(id, "N/A (sampling pending)");
    if (hasTarget || !output.values.count("graphics_api")) {
        output.values["graphics_api"] = "N/A (sampling pending)";
        output.values["graphics_api_status"] = c.has("graphics_api") ? "waiting" : "disabled";
    }
    for (std::size_t i = 1; i < Count; ++i) {
        bool fresh = false;
        const auto expected = i >= 6 ? revision : s.epoch;
        if (auto result = s.lanes[i]->take(); result && result->epoch == expected) {
            s.cache[i] = std::move(result); fresh = true;
        }
        if (s.cache[i]) {
            const auto age = std::max<std::int64_t>(0, now - s.cache[i]->started) / 1000000;
            output.values[std::string("sample_age_ms_") + Names[i]] = std::to_string(age);
            output.values[std::string("sample_duration_ms_") + Names[i]] = std::to_string((s.cache[i]->finished - s.cache[i]->started) / 1000000);
            /** @brief 超过两周期标记迟滞，十五秒到期；不因聚合发布而刷新通道采样时间。 */
            /** @brief 屏幕和权限状态保留最后已知值；过期只清除数值，不假定屏幕重新亮起。 */
            if (i == 7) output.platform = s.cache[i]->packet.platform;
            if (age <= 15000) {
                for (const auto &item : s.cache[i]->packet.values) output.values[item.first] = item.second;
                if (i == 1) {
                    output.frames = s.cache[i]->packet.frames;
                    if (!fresh) { output.frames.intervals.clear(); output.frames.resetHistory = false; output.frames.truncated = false; }
                }
            }
            if (age > c.interval * 2) {
                if (!output.delays.empty()) output.delays += ", ";
                output.delays += std::string(Names[i]) + " " + std::to_string(age) + "ms";
            }
        }
        if (i == 7 || (awake && (i == 6 || hasTarget))) s.lanes[i]->submit({c, s.target, expected});
    }
    if (awake) s.lanes[0]->submit({c, {}, revision});
    output.values["refresh_interval_ms"] = std::to_string(c.interval);
    output.values["sampling_delays"] = output.delays.empty() ? "none" : output.delays;
    return output;
}
}
