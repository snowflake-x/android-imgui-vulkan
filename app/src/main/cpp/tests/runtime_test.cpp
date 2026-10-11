#include "plugin/runtime.h"

#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using vulkanscope::plugin::Context;
using vulkanscope::plugin::Runtime;
using vulkanscope::plugin::ServiceHandle;

struct Tick {
    int value = 1;
};

struct Counter {
    int value = 0;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Action>
void requireThrows(Action&& action, const char* message) {
    try {
        std::forward<Action>(action)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void loadUnloadReload() {
    int setups = 0;
    int callbacks = 0;
    int resources = 0;
    Runtime runtime;
    runtime.add({"feature", {}, [&](Context& context) {
                     ++setups;
                     context.defer([&]() noexcept { --resources; });
                     ++resources;
                     context.on<Tick>([&](const Tick& tick) { callbacks += tick.value; });
                 }});

    require(runtime.contains("feature"), "registered plugin must be discoverable");
    require(!runtime.isLoaded("feature"), "registration must not activate a plugin");
    require(resources == 0, "registration must not acquire resources");
    runtime.load("feature");
    runtime.load("feature");
    require(setups == 1 && resources == 1, "repeated load must be idempotent");
    require(runtime.loadedCount() == 1, "loaded count must describe active plugins");
    runtime.emit(Tick{3});
    require(callbacks == 3, "active callback must receive its event");
    require(runtime.unload("feature"), "first unload must report an active plugin");
    require(!runtime.unload("feature"), "repeated unload must be idempotent");
    runtime.emit(Tick{7});
    require(callbacks == 3 && resources == 0, "unload must revoke callbacks and resources");
    runtime.load("feature");
    runtime.emit(Tick{2});
    require(setups == 2 && callbacks == 5 && resources == 1,
            "reload must create one fresh activation");
    runtime.clear();
    runtime.clear();
    require(resources == 0 && runtime.loadedCount() == 0, "clear must be idempotent");
}

void failedSetupLeavesNoEffects() {
    int resources = 0;
    int callbacks = 0;
    int attempts = 0;
    std::weak_ptr<Counter> ownedService;
    Runtime runtime;
    runtime.add({"broken", {}, [&](Context& context) {
                     ++attempts;
                     context.defer([&]() noexcept { --resources; });
                     ++resources;
                     context.on<Tick>([&](const Tick&) { ++callbacks; });
                     auto service = std::make_shared<Counter>();
                     ownedService = service;
                     context.provide(service);
                     throw std::runtime_error("setup failed");
                 }});

    for (int attempt = 0; attempt != 2; ++attempt) {
        requireThrows<std::runtime_error>([&] { runtime.load("broken"); },
                                          "setup error must propagate");
        require(resources == 0 && ownedService.expired(), "failed load must release all ownership");
        require(!runtime.isLoaded("broken") && runtime.loadedCount() == 0,
                "failed load must not leave an activation");
        runtime.emit(Tick{});
        require(callbacks == 0, "failed load must not leave an event subscription");
    }
    require(attempts == 2, "failed activation must be retryable");

    runtime.add({"replacement", {}, [](Context& context) {
                     context.provide(std::make_shared<Counter>());
                 }});
    runtime.load("replacement");
    require(runtime.isLoaded("replacement"), "failed service publication must not reserve its type");
}

void failedDependencyTransactionPreservesExistingPlugins() {
    int existingResources = 0;
    int transientResources = 0;
    int existingCallbacks = 0;
    std::vector<std::string> cleanup;
    Runtime runtime;
    runtime.add({"existing", {}, [&](Context& context) {
                     context.defer([&]() noexcept { --existingResources; });
                     ++existingResources;
                     context.on<Tick>([&](const Tick&) { ++existingCallbacks; });
                 }});
    runtime.add({"transient", {}, [&](Context& context) {
                     context.defer([&]() noexcept {
                         --transientResources;
                         cleanup.emplace_back("transient");
                     });
                     ++transientResources;
                     context.provide(std::make_shared<Counter>());
                 }});
    runtime.add({"root", {"existing", "transient"}, [&](Context& context) {
                     context.defer([&]() noexcept { cleanup.emplace_back("root"); });
                     context.service<Counter>().borrow([](Counter& counter) { ++counter.value; });
                     throw std::runtime_error("root failed");
                 }});
    runtime.load("existing");
    requireThrows<std::runtime_error>([&] { runtime.load("root"); },
                                      "dependent setup error must propagate");
    require(runtime.loadedCount() == 1 && runtime.isLoaded("existing"),
            "rollback must preserve preexisting activations");
    require(!runtime.isLoaded("transient") && !runtime.isLoaded("root"),
            "rollback must remove newly loaded dependency graph");
    require(existingResources == 1 && transientResources == 0,
            "rollback must release only resources from the failed transaction");
    require(cleanup == std::vector<std::string>({"root", "transient"}),
            "transaction rollback must run dependents before dependencies");
    runtime.emit(Tick{});
    require(existingCallbacks == 1, "existing callbacks must survive another graph's failure");
}

void missingDependenciesAndCyclesHaveNoEffects() {
    int setups = 0;
    Runtime runtime;
    runtime.add({"healthy", {}, [&](Context&) { ++setups; }});
    runtime.add({"missing", {"absent"}, [&](Context&) { ++setups; }});
    runtime.add({"cycle-a", {"cycle-b"}, [&](Context&) { ++setups; }});
    runtime.add({"cycle-b", {"cycle-a"}, [&](Context&) { ++setups; }});
    runtime.add({"mixed-missing", {"healthy", "missing"}, [&](Context&) { ++setups; }});
    runtime.add({"mixed-cycle", {"healthy", "cycle-a"}, [&](Context&) { ++setups; }});
    requireThrows<std::logic_error>([&] { runtime.load("missing"); },
                                    "missing dependency must be rejected");
    requireThrows<std::logic_error>([&] { runtime.load("cycle-a"); },
                                    "dependency cycle must be rejected");
    requireThrows<std::logic_error>([&] { runtime.load("absent"); },
                                    "unknown plugin must be rejected");
    requireThrows<std::logic_error>([&] { runtime.load("mixed-missing"); },
                                    "missing dependency in a later branch must be rejected");
    requireThrows<std::logic_error>([&] { runtime.load("mixed-cycle"); },
                                    "dependency cycle in a later branch must be rejected");
    require(setups == 0 && runtime.loadedCount() == 0,
            "invalid dependency graphs must be rejected before setup");
}

void invalidDefinitionsDoNotReplacePlugins() {
    int callbacks = 0;
    Runtime runtime;
    runtime.add({"original", {}, [&](Context& context) {
                     context.on<Tick>([&](const Tick&) { ++callbacks; });
                 }});
    requireThrows<std::invalid_argument>(
            [&] { runtime.add({"original", {}, [](Context&) {}}); },
            "duplicate definitions must be rejected");
    requireThrows<std::invalid_argument>([&] { runtime.add({"", {}, [](Context&) {}}); },
                                         "empty plugin id must be rejected");
    requireThrows<std::invalid_argument>([&] { runtime.add({"no-setup", {}, {}}); },
                                         "missing setup must be rejected");
    runtime.load("original");
    runtime.emit(Tick{});
    require(callbacks == 1, "invalid definition must preserve the original definition");
}

void serviceConflictsRollbackWithoutReplacingOwner() {
    ServiceHandle<Counter> handle;
    int conflictingResources = 0;
    Runtime runtime;
    runtime.add({"owner", {}, [](Context& context) {
                     context.provide(std::make_shared<Counter>(Counter{42}));
                 }});
    runtime.add({"consumer", {"owner"}, [&](Context& context) {
                     handle = context.service<Counter>();
                 }});
    runtime.add({"conflict", {}, [&](Context& context) {
                     context.defer([&]() noexcept { --conflictingResources; });
                     ++conflictingResources;
                     context.provide(std::make_shared<Counter>(Counter{99}));
                 }});
    runtime.load("consumer");
    requireThrows<std::logic_error>([&] { runtime.load("conflict"); },
                                    "duplicate service publication must be rejected");
    require(conflictingResources == 0 && runtime.loadedCount() == 2,
            "service conflict must roll back its activation");
    handle.borrow([](Counter& counter) {
        require(counter.value == 42, "conflicting service must not replace the active provider");
    });
    runtime.add({"null-service", {}, [](Context& context) {
                     context.provide(std::shared_ptr<Counter>{});
                 }});
    requireThrows<std::invalid_argument>([&] { runtime.load("null-service"); },
                                         "null service must be rejected");
}

void servicesRequireDeclaredDependencies() {
    ServiceHandle<Counter> transitive;
    Runtime runtime;
    runtime.add({"provider", {}, [](Context& context) {
                     context.provide(std::make_shared<Counter>(Counter{17}));
                 }});
    runtime.add({"middle", {"provider"}, [](Context&) {}});
    runtime.add({"consumer", {"middle"}, [&](Context& context) {
                     transitive = context.service<Counter>();
                 }});
    runtime.add({"undeclared", {}, [](Context& context) {
                     context.service<Counter>().borrow([](Counter&) {});
                 }});
    runtime.load("consumer");
    transitive.borrow([](Counter& counter) {
        require(counter.value == 17, "transitive dependency must expose its service");
    });
    requireThrows<std::logic_error>([&] { runtime.load("undeclared"); },
                                    "unrelated plugin must not borrow an undeclared service");
    require(runtime.loadedCount() == 3, "undeclared access must not disturb loaded dependencies");
}

void dependencyUnloadAndClearOrder() {
    std::vector<std::string> cleanup;
    Runtime runtime;
    const auto setup = [&](const std::string& id) {
        return [&, id](Context& context) {
            context.defer([&, id]() noexcept { cleanup.emplace_back(id + "-first"); });
            context.defer([&, id]() noexcept { cleanup.emplace_back(id + "-second"); });
        };
    };
    runtime.add({"leaf", {}, setup("leaf")});
    runtime.add({"middle", {"leaf"}, setup("middle")});
    runtime.add({"root", {"middle"}, setup("root")});
    runtime.load("root");
    requireThrows<std::logic_error>([&] { runtime.unload("leaf"); },
                                    "active dependent must prevent dependency unload");
    requireThrows<std::logic_error>([&] { runtime.unload("middle"); },
                                    "direct dependent must prevent dependency unload");
    require(cleanup.empty() && runtime.loadedCount() == 3,
            "rejected unload must leave the entire graph active");
    runtime.clear();
    require(cleanup == std::vector<std::string>({"root-second", "root-first", "middle-second",
                                                "middle-first", "leaf-second", "leaf-first"}),
            "clear must dispose in reverse dependency and effect registration order");
}

void unloadingRootsReleasesOnlyUnusedAutomaticDependencies() {
    int leafSetups = 0;
    int leafResources = 0;
    Runtime runtime;
    runtime.add({"leaf", {}, [&](Context& context) {
                     ++leafSetups;
                     context.defer([&]() noexcept { --leafResources; });
                     ++leafResources;
                 }});
    runtime.add({"first", {"leaf"}, [](Context&) {}});
    runtime.add({"second", {"leaf"}, [](Context&) {}});
    runtime.load("first");
    runtime.load("second");
    require(leafSetups == 1 && runtime.loadedCount() == 3,
            "shared dependencies must activate only once");
    runtime.unload("first");
    require(runtime.isLoaded("leaf") && runtime.isLoaded("second") && leafResources == 1,
            "remaining dependent must keep the shared dependency alive");
    runtime.unload("second");
    require(runtime.loadedCount() == 0 && leafResources == 0,
            "last root unload must restore the pre-load resource baseline");

    runtime.load("leaf");
    runtime.load("first");
    runtime.unload("first");
    require(runtime.loadedCount() == 1 && runtime.isLoaded("leaf") && leafResources == 1,
            "root unload must preserve a previously explicit dependency");
    runtime.unload("leaf");

    runtime.load("first");
    runtime.load("leaf");
    runtime.unload("first");
    require(runtime.loadedCount() == 1 && runtime.isLoaded("leaf") && leafResources == 1,
            "explicit load must pin an already automatic dependency");
    runtime.unload("leaf");
    require(leafResources == 0, "explicit dependency must still support direct unload");
}

void failedLoadDoesNotPinExistingAutomaticDependencies() {
    int leafResources = 0;
    Runtime runtime;
    runtime.add({"leaf", {}, [&](Context& context) {
                     context.defer([&]() noexcept { --leafResources; });
                     ++leafResources;
                 }});
    runtime.add({"existing-root", {"leaf"}, [](Context&) {}});
    runtime.add({"failed-root", {"leaf"}, [](Context&) {
                     throw std::runtime_error("root failed");
                 }});
    runtime.load("existing-root");
    requireThrows<std::runtime_error>([&] { runtime.load("failed-root"); },
                                      "failed root load must propagate its error");
    require(runtime.loadedCount() == 2 && leafResources == 1,
            "failed load must leave the existing automatic dependency untouched");
    runtime.unload("existing-root");
    require(runtime.loadedCount() == 0 && leafResources == 0,
            "failed transaction must not pin an existing automatic dependency");
}

void staleHandlesNeverReconnectAfterReload() {
    ServiceHandle<Counter> current;
    ServiceHandle<Counter> stale;
    int generations = 0;
    Runtime runtime;
    runtime.add({"provider", {}, [&](Context& context) {
                     context.provide(std::make_shared<Counter>(Counter{++generations}));
                 }});
    runtime.add({"consumer", {"provider"}, [&](Context& context) {
                     current = context.service<Counter>();
                 }});
    runtime.load("consumer");
    stale = current;
    runtime.unload("consumer");
    requireThrows<std::logic_error>([&] { stale.borrow([](Counter&) {}); },
                                    "consumer disposal must invalidate its service handles");
    runtime.unload("provider");
    runtime.load("consumer");
    current.borrow([](Counter& counter) {
        require(counter.value == 2, "reload must return the new service generation");
    });
    requireThrows<std::logic_error>([&] { stale.borrow([](Counter&) {}); },
                                    "old service handles must never reconnect after reload");
    runtime.clear();
    requireThrows<std::logic_error>([&] { current.borrow([](Counter&) {}); },
                                    "clear must invalidate all service handles");
}

void handlesOutliveRuntimeSafely() {
    ServiceHandle<Counter> stale;
    {
        Runtime runtime;
        runtime.add({"owner", {}, [&](Context& context) {
                         context.provide(std::make_shared<Counter>());
                         stale = context.service<Counter>();
                     }});
        runtime.load("owner");
    }
    requireThrows<std::logic_error>([&] { stale.borrow([](Counter&) {}); },
                                    "runtime destruction must safely revoke service handles");
    ServiceHandle<Counter> empty;
    requireThrows<std::logic_error>([&] { empty.borrow([](Counter&) {}); },
                                    "empty service handle must fail deterministically");
}

void eventDispatchRejectsLifecycleReentry() {
    int rejectionCount = 0;
    int callbacks = 0;
    Runtime runtime;
    runtime.add({"idle", {}, [](Context&) {}});
    runtime.add({"listener", {}, [&](Context& context) {
                     context.on<Tick>([&](const Tick&) {
                         ++callbacks;
                         const auto rejected = [&](const auto& action) {
                             try {
                                 action();
                             } catch (const std::logic_error&) {
                                 ++rejectionCount;
                             }
                         };
                         rejected([&] { runtime.load("idle"); });
                         rejected([&] { runtime.unload("listener"); });
                         rejected([&] { runtime.add({"during-dispatch", {}, [](Context&) {}}); });
                         rejected([&] { runtime.emit(Tick{}); });
                         rejected([&] { runtime.clear(); });
                     });
                 }});
    runtime.load("listener");
    runtime.emit(Tick{});
    require(rejectionCount == 5 && callbacks == 1,
            "dispatch must reject lifecycle changes and recursive dispatch");
    require(runtime.isLoaded("listener") && !runtime.isLoaded("idle") &&
                    !runtime.contains("during-dispatch"),
            "rejected dispatch mutation must have no effects");
    runtime.unload("listener");
    runtime.load("idle");
    require(runtime.isLoaded("idle"), "dispatch guard must release after delivery");
}

void cleanupRejectsLifecycleReentry() {
    int rejectionCount = 0;
    Runtime runtime;
    runtime.add({"idle", {}, [](Context&) {}});
    runtime.add({"disposer", {}, [&](Context& context) {
                     context.defer([&]() noexcept {
                         const auto rejected = [&](const auto& action) {
                             try {
                                 action();
                             } catch (const std::logic_error&) {
                                 ++rejectionCount;
                             }
                         };
                         rejected([&] { runtime.load("idle"); });
                         rejected([&] { runtime.unload("disposer"); });
                         rejected([&] { runtime.add({"during-cleanup", {}, [](Context&) {}}); });
                         rejected([&] { runtime.emit(Tick{}); });
                         rejected([&] { runtime.clear(); });
                     });
                 }});
    runtime.load("disposer");
    runtime.unload("disposer");
    require(rejectionCount == 5, "cleanup must reject lifecycle and dispatch reentry");
    require(runtime.loadedCount() == 0 && !runtime.contains("during-cleanup"),
            "cleanup reentry must not introduce activations or definitions");
    runtime.load("idle");
    require(runtime.isLoaded("idle"), "cleanup guard must release after disposal");
}

void callbackExceptionDoesNotPoisonRuntime() {
    Runtime runtime;
    runtime.add({"throwing", {}, [](Context& context) {
                     context.on<Tick>([](const Tick&) { throw std::runtime_error("callback failed"); });
                 }});
    runtime.add({"healthy", {}, [](Context&) {}});
    runtime.load("throwing");
    requireThrows<std::runtime_error>([&] { runtime.emit(Tick{}); },
                                      "event callback exception must propagate");
    runtime.unload("throwing");
    runtime.load("healthy");
    require(runtime.isLoaded("healthy"), "callback exception must restore idle lifecycle state");
}

void contextRegistrationEndsAfterSetup() {
    int rejected = 0;
    int serviceBorrows = 0;
    Runtime runtime;
    runtime.add({"listener", {}, [&](Context& context) {
                     context.provide(std::make_shared<Counter>());
                     context.on<Tick>([&, contextPointer = &context](const Tick&) {
                         requireThrows<std::logic_error>(
                                 [&] { contextPointer->on<Tick>([](const Tick&) {}); },
                                 "event registration after setup must be rejected");
                         ++rejected;
                         requireThrows<std::logic_error>(
                                 [&] { contextPointer->provide(std::make_shared<Counter>()); },
                                 "service registration after setup must be rejected");
                         ++rejected;
                         requireThrows<std::logic_error>(
                                 [&] { contextPointer->defer([]() noexcept {}); },
                                 "effect registration after setup must be rejected");
                         ++rejected;
                         contextPointer->service<Counter>().borrow([&](Counter& counter) {
                             ++counter.value;
                             ++serviceBorrows;
                         });
                     });
                 }});
    runtime.load("listener");
    runtime.emit(Tick{});
    runtime.emit(Tick{});
    require(rejected == 6 && serviceBorrows == 2,
            "active context must allow service use while rejecting new setup effects");
}

void borrowingPreventsServiceDestruction() {
    ServiceHandle<Counter> handle;
    Runtime runtime;
    runtime.add({"owner", {}, [&](Context& context) {
                     context.provide(std::make_shared<Counter>());
                     handle = context.service<Counter>();
                 }});
    runtime.load("owner");
    handle.borrow([&](Counter& counter) {
        requireThrows<std::logic_error>([&] { runtime.unload("owner"); },
                                        "borrowed service must not be destroyed reentrantly");
        requireThrows<std::logic_error>([&] { runtime.clear(); },
                                        "clear must not destroy borrowed services");
        ++counter.value;
    });
    handle.borrow([](Counter& counter) {
        require(counter.value == 1, "rejected disposal must preserve borrowed service");
    });
    runtime.unload("owner");
}

void failedAcquisitionRunsPreRegisteredCompensation() {
    int resources = 0;
    int cleanupCalls = 0;
    Runtime runtime;
    runtime.add({"partial", {}, [&](Context& context) {
                     context.effect(
                             [&] {
                                 ++resources;
                                 throw std::runtime_error("partially acquired resource");
                             },
                             [&]() noexcept {
                                 --resources;
                                 ++cleanupCalls;
                             });
                 }});
    requireThrows<std::runtime_error>([&] { runtime.load("partial"); },
                                      "acquisition failure must propagate");
    require(resources == 0 && cleanupCalls == 1 && runtime.loadedCount() == 0,
            "partial acquisition must run its compensation exactly once");
    runtime.clear();
    require(cleanupCalls == 1, "failed effect must not retain a second cleanup");
}

void crossThreadAccessIsRejected() {
    ServiceHandle<Counter> handle;
    Runtime runtime;
    runtime.add({"owner", {}, [&](Context& context) {
                     context.provide(std::make_shared<Counter>(Counter{7}));
                     handle = context.service<Counter>();
                 }});
    runtime.load("owner");
    int rejected = 0;
    std::exception_ptr unexpected;
    std::thread other([&] {
        try {
            requireThrows<std::logic_error>([&] { runtime.load("owner"); },
                                            "lifecycle access must enforce the owner thread");
            ++rejected;
            requireThrows<std::logic_error>([&] { runtime.emit(Tick{}); },
                                            "event dispatch must enforce the owner thread");
            ++rejected;
            requireThrows<std::logic_error>([&] { handle.borrow([](Counter&) {}); },
                                            "service borrow must enforce the owner thread");
            ++rejected;
        } catch (...) {
            unexpected = std::current_exception();
        }
    });
    other.join();
    if (unexpected) {
        std::rethrow_exception(unexpected);
    }
    require(rejected == 3 && runtime.loadedCount() == 1,
            "cross-thread rejection must preserve the active graph");
    handle.borrow([](Counter& counter) {
        require(counter.value == 7, "cross-thread rejection must preserve service state");
    });
}

}

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
            {"load, unload, and reload", loadUnloadReload},
            {"failed setup leaves no effects", failedSetupLeavesNoEffects},
            {"transaction rollback preserves existing plugins", failedDependencyTransactionPreservesExistingPlugins},
            {"missing dependencies and cycles", missingDependenciesAndCyclesHaveNoEffects},
            {"invalid definitions preserve originals", invalidDefinitionsDoNotReplacePlugins},
            {"service conflict rollback", serviceConflictsRollbackWithoutReplacingOwner},
            {"declared service dependencies", servicesRequireDeclaredDependencies},
            {"dependency unload and cleanup order", dependencyUnloadAndClearOrder},
            {"automatic dependency ownership", unloadingRootsReleasesOnlyUnusedAutomaticDependencies},
            {"failed load preserves dependency ownership", failedLoadDoesNotPinExistingAutomaticDependencies},
            {"stale handles across reload", staleHandlesNeverReconnectAfterReload},
            {"handles outlive runtime", handlesOutliveRuntimeSafely},
            {"dispatch reentry", eventDispatchRejectsLifecycleReentry},
            {"cleanup reentry", cleanupRejectsLifecycleReentry},
            {"callback exception recovery", callbackExceptionDoesNotPoisonRuntime},
            {"context registration lifetime", contextRegistrationEndsAfterSetup},
            {"service borrow disposal guard", borrowingPreventsServiceDestruction},
            {"partial acquisition compensation", failedAcquisitionRunsPreRegisteredCompensation},
            {"owner thread contract", crossThreadAccessIsRejected},
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " plugin runtime tests passed\n";
    return failures == 0 ? 0 : 1;
}
