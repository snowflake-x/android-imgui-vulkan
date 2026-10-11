#pragma once

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

/** @brief Reversible, owner-thread plugin lifetimes for native features. */
namespace vulkanscope::plugin {

class Context;
class Runtime;

namespace detail {
struct Scope;
struct ServiceEntry;

/** @brief Validate both lifetimes before a synchronous service borrow. */
void borrowService(const std::weak_ptr<Scope>& consumer,
                   const std::weak_ptr<ServiceEntry>& entry,
                   const std::function<void(void*)>& callback);
}

/**
 * @brief A revocable service capability, never an owning service pointer.
 * @note Do not retain references obtained by borrow() beyond its callback.
 */
template <typename T>
class ServiceHandle {
public:
    ServiceHandle() = default;

    /**
     * @brief Use the service while provider and consumer are both alive.
     * @note The callback must return void. Lifecycle mutations are forbidden
     *       until it returns; exceptions propagate to the caller.
     */
    template <typename F>
    void borrow(F&& callback) const {
        static_assert(std::is_same_v<std::invoke_result_t<F, T&>, void>,
                      "Service borrows must return void, not an escaping value");
        detail::borrowService(consumer_, entry_, [&](void* value) {
            std::invoke(std::forward<F>(callback), *static_cast<T*>(value));
        });
    }

private:
    friend class Context;
    ServiceHandle(std::weak_ptr<detail::Scope> consumer,
                  std::weak_ptr<detail::ServiceEntry> entry)
        : consumer_(std::move(consumer)), entry_(std::move(entry)) {}

    std::weak_ptr<detail::Scope> consumer_;
    std::weak_ptr<detail::ServiceEntry> entry_;
};

/** @brief A registered plugin definition; dependencies are loaded first. */
struct PluginSpec {
    std::string id;
    std::vector<std::string> dependencies;
    std::function<void(Context&)> setup;
};

/**
 * @brief Ownership boundary for one activation of a plugin.
 * @note Registrations are permitted only during this plugin's setup. The
 *       context lives until unload; store ServiceHandle for revocable access.
 */
class Context {
public:
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    /** @brief Register a typed listener owned by this activation. */
    template <typename Event>
    void on(std::function<void(const Event&)> callback) {
        if (!callback) {
            rejectEmptyCallback();
        }
        addListener(typeid(Event), [callback = std::move(callback)](const void* event) {
            callback(*static_cast<const Event*>(event));
        });
    }

    /** @brief Publish a service after the complete load transaction commits. */
    template <typename T>
    void provide(std::shared_ptr<T> service) {
        static_assert(!std::is_const_v<T>, "Register an unqualified service type");
        addService(typeid(T), std::move(service));
    }

    /** @brief Resolve an own service or a declared transitive dependency. */
    template <typename T>
    ServiceHandle<T> service() {
        static_assert(!std::is_const_v<T>, "Resolve an unqualified service type");
        return ServiceHandle<T>(scope_, findService(typeid(T)));
    }

    /**
     * @brief Register rollback before acquiring an external resource.
     * @note Cleanup must tolerate incomplete acquisition and cannot throw.
     */
    template <typename F>
    void defer(F&& cleanup) {
        static_assert(std::is_nothrow_invocable_v<std::decay_t<F>&>,
                      "Plugin cleanup must be noexcept");
        addCleanup(std::forward<F>(cleanup));
    }

    /** @brief Install rollback first, then perform the resource acquisition. */
    template <typename Acquire, typename Cleanup>
    void effect(Acquire&& acquire, Cleanup&& cleanup) {
        defer(std::forward<Cleanup>(cleanup));
        std::invoke(std::forward<Acquire>(acquire));
    }

private:
    friend class Runtime;
    Context(Runtime& runtime, std::weak_ptr<detail::Scope> scope)
        : runtime_(runtime), scope_(std::move(scope)) {}

    static void rejectEmptyCallback();
    void addListener(std::type_index type, std::function<void(const void*)> callback);
    void addService(std::type_index type, std::shared_ptr<void> service);
    std::weak_ptr<detail::ServiceEntry> findService(std::type_index type);
    void addCleanup(std::function<void()> cleanup);

    Runtime& runtime_;
    std::weak_ptr<detail::Scope> scope_;
};

/**
 * @brief Atomic managed registrations with reversible dependency lifetimes.
 * @note All access and destruction must occur on the constructing thread.
 *       Reentrant lifecycle changes and event emission throw logic_error.
 *       Arbitrary native I/O is reversible only through plugin cleanup.
 */
class Runtime {
public:
    Runtime();
    ~Runtime() noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    /** @brief Register a unique definition without executing plugin code. */
    void add(PluginSpec spec);

    /**
     * @brief Load dependencies transactionally and pin the requested plugin.
     * @note Dependencies loaded implicitly are collected when no longer used.
     */
    void load(const std::string& id);

    /**
     * @brief Unload a plugin and unused automatic dependencies.
     * @note Rejects requests with active dependents; explicitly loaded
     *       dependencies remain until separately unloaded or cleared.
     * @return False if the plugin is already inactive or unregistered.
     */
    bool unload(const std::string& id);

    /** @brief Unload all activations in reverse dependency order. */
    void clear();

    /** @brief Whether a definition has been registered. */
    bool contains(const std::string& id) const;
    /** @brief Whether an activation has committed successfully. */
    bool isLoaded(const std::string& id) const;
    /** @brief Number of committed activations. */
    std::size_t loadedCount() const;

    /** @brief Dispatch an immutable event synchronously on the owner thread. */
    template <typename Event>
    void emit(const Event& event) {
        emitErased(typeid(Event), &event);
    }

private:
    friend class Context;
    friend void detail::borrowService(const std::weak_ptr<detail::Scope>&,
                                      const std::weak_ptr<detail::ServiceEntry>&,
                                      const std::function<void(void*)>&);
    struct Impl;
    std::unique_ptr<Impl> impl_;

    void emitErased(std::type_index type, const void* event);
};

}
