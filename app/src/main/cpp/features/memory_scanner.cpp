#include "features/memory_scanner.h"
#include <mutex>
#include <sstream>
#include <thread>

namespace vulkanscope {
namespace {
/** @brief No host pointers cross the worker boundary; unload cancels and joins before releasing state. */
class Worker {
public:
    explicit Worker(MemoryExecutor executor) : execute_(std::move(executor)) {}
    ~Worker() { stop(); }
    void stop() noexcept {
        cancelled_ = true;
        if (thread_.joinable()) thread_.join();
    }
    MemoryState snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }
    bool action(const MemoryJob &job) {
        if (job.operation == "cancel") { cancelled_ = true; return true; }
        if (!execute_ || (job.operation != "list" && job.operation != "scan" && job.operation != "unity" && job.operation != "view")) return false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.busy) return false;
            if (job.operation != "list") {
                bool known = false;
                for (const auto &p : state_.processes) if (p.pid == job.pid && p.start == job.start) known = true;
                if (!known) return false;
            }
        }
        if (job.operation == "scan") {
            try { (void)memory::Pattern(job.kind, job.input); } catch (const std::exception &) { return false; }
        }
        stop();
        cancelled_ = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previousIdentity_ = state_.viewIdentity; previousHex_ = state_.viewHex;
            state_.viewAddress.clear(); state_.viewHex.clear(); state_.viewIdentity.clear();
            state_.changed.clear(); state_.pointers.clear();
            const auto identity = std::to_string(job.pid) + ":" + std::to_string(job.start);
            if (job.operation != "view" || state_.targetIdentity != identity) state_.matches.clear();
            state_.targetIdentity = job.operation == "list" ? "" : std::to_string(job.pid) + ":" + std::to_string(job.start);
            state_.busy = true; state_.status = "running"; state_.report.clear();
        }
        try {
            thread_ = std::thread([this, job] {
                monitor::CommandResult result;
                try { result = execute_(job, cancelled_); }
                catch (const std::exception &e) { result.error = e.what(); }
                catch (...) { result.error = "Unexpected memory worker failure"; }
                const auto marker = result.output.rfind("MEMSCAN_DONE\n");
                const bool complete = result.success && !result.truncated && marker != std::string::npos &&
                    marker + 13 == result.output.size();
                std::lock_guard<std::mutex> lock(mutex_);
                state_.busy = false;
                state_.status = cancelled_ ? "cancelled" : complete ? "finished" : "failed";
                if (cancelled_) { state_.report = "Cancelled"; return; }
                state_.report = complete ? result.output.substr(0, marker) : result.error + "\n" + result.output;
                if (!complete) {
                    state_.report += "\nIncomplete helper response; do not treat this as a complete scan.";
                    return;
                }
                const auto oldIdentity = previousIdentity_, oldHex = previousHex_;
                state_.viewAddress.clear(); state_.viewHex.clear(); state_.changed.clear(); state_.pointers.clear();
                if (job.operation != "view") state_.matches.clear();
                if (job.operation == "view") {
                    state_.viewIdentity = std::to_string(job.pid) + ":" + std::to_string(job.start);
                    std::istringstream lines(state_.report);
                    std::ostringstream clean;
                    std::string line;
                    while (std::getline(lines, line)) {
                        if (line.rfind("VIEW\t", 0) == 0) {
                            std::istringstream fields(line.substr(5));
                            fields >> state_.viewAddress >> state_.viewHex;
                            state_.viewIdentity += ":" + state_.viewAddress;
                        } else if (line.rfind("PTR\t", 0) == 0 && state_.pointers.size() < 128) {
                            std::istringstream fields(line.substr(4));
                            MemoryPointer pointer;
                            if (fields >> pointer.address >> pointer.label) state_.pointers.push_back(std::move(pointer));
                        } else clean << line << '\n';
                    }
                    state_.report = clean.str();
                    if (state_.viewHex.size() > 8192 || state_.viewHex.size() % 2 ||
                        state_.viewHex.find_first_not_of("0123456789abcdef") != std::string::npos)
                        state_.viewHex.clear();
                    if (oldIdentity == state_.viewIdentity)
                        for (std::size_t i = 0; i + 1 < std::min(oldHex.size(), state_.viewHex.size()); i += 2)
                            if (oldHex.compare(i, 2, state_.viewHex, i, 2) != 0) state_.changed.push_back(i / 2);
                } else state_.viewIdentity.clear();
                if (job.operation == "scan") {
                    std::istringstream lines(state_.report);
                    std::string line;
                    while (std::getline(lines, line))
                        if (line.rfind("0x", 0) == 0 && line.size() <= 18 &&
                            line.substr(2).find_first_not_of("0123456789abcdef") == std::string::npos &&
                            state_.matches.size() < 1024) state_.matches.push_back(line);
                }
                if (job.operation == "list") {
                    state_.processes.clear();
                    std::istringstream lines(state_.report);
                    std::string line;
                    while (std::getline(lines, line)) {
                        if (line.rfind("P\t", 0) != 0 || state_.processes.size() >= 2048) continue;
                        std::istringstream fields(line.substr(2));
                        memory::Process p;
                        if (fields >> p.pid >> p.start >> p.uid >> p.name && p.pid > 0 && p.start)
                            state_.processes.push_back(std::move(p));
                    }
                    const bool partial = state_.report.find("(partial)") != std::string::npos;
                    state_.report = "App processes: " + std::to_string(state_.processes.size()) + (partial ? " (partial)" : "");
                }
            });
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.busy = false; state_.status = "failed"; state_.report = "Cannot start memory worker";
            return false;
        }
        return true;
    }
private:
    MemoryExecutor execute_;
    std::mutex mutex_;
    MemoryState state_;
    std::string previousIdentity_, previousHex_;
    std::atomic<bool> cancelled_{false};
    std::thread thread_;
};
}
void RegisterMemoryScanner(plugin::Runtime &runtime, MemoryExecutor execute) {
    runtime.add({"memory-scanner", {}, [execute = std::move(execute)](plugin::Context &context) {
        auto worker = std::make_shared<Worker>(execute);
        context.defer([worker]() noexcept { worker->stop(); });
        context.on<MemoryAction>([worker](const MemoryAction &action) {
            *action.accepted = worker->action(action.job);
        });
        context.on<MemoryQuery>([worker](const MemoryQuery &query) { *query.snapshot = worker->snapshot(); });
    }});
}
}
