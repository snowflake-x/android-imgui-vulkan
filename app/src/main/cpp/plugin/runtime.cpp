#include "runtime.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace vulkanscope::plugin {

namespace detail {

/** @brief A service value whose availability is revoked before cleanup. */
struct ServiceEntry {
    std::type_index type;
    std::shared_ptr<void> value;
    std::weak_ptr<Scope> provider;
    bool available = true;
};

/** @brief One activation's resources, callbacks and rollback stack. */
struct Scope {
    Runtime* runtime;
    std::thread::id owner;
    std::string id;
    bool alive = true;
    bool committed = false;
    std::vector<std::pair<std::type_index, std::function<void(const void*)>>> listeners;
    std::vector<std::shared_ptr<ServiceEntry>> services;
    std::vector<std::function<void()>> cleanup;
};

}

/** @brief Internal state stays platform independent and single threaded. */
struct Runtime::Impl {
    enum class Phase { idle, loading, unloading, emitting };
    enum class State { inactive, visiting, staging, active };

    /** @brief Stable definition plus its current activation and explicit pin. */
    struct Record {
        PluginSpec spec;
        State state = State::inactive;
        bool pinned = false;
        std::shared_ptr<detail::Scope> scope;
        std::unique_ptr<Context> context;
    };

    const std::thread::id owner = std::this_thread::get_id();
    Phase phase = Phase::idle;
    std::size_t borrows = 0;
    Record* settingUp = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Record>> records;
    std::vector<Record*> loaded;

    /** @brief Restore state on callback or setup exceptions. */
    struct PhaseGuard {
        Impl& impl;
        explicit PhaseGuard(Impl& value, Phase next) : impl(value) { impl.phase = next; }
        ~PhaseGuard() { impl.phase = Phase::idle; impl.settingUp = nullptr; }
    };

    void checkOwner() const {
        if (std::this_thread::get_id() != owner) {
            throw std::logic_error("Plugin runtime used outside its owner thread");
        }
    }

    void checkIdle() const {
        checkOwner();
        if (phase != Phase::idle || borrows != 0) {
            throw std::logic_error("Reentrant plugin lifecycle or event dispatch");
        }
    }

    std::shared_ptr<detail::Scope> checkScope(const std::weak_ptr<detail::Scope>& weak,
                                            bool registration) const {
        checkOwner();
        auto scope = weak.lock();
        if (!scope || !scope->alive || phase == Phase::unloading) {
            throw std::logic_error("Plugin context has been revoked");
        }
        if (registration && (phase != Phase::loading || !settingUp ||
                             settingUp->scope != scope || borrows != 0)) {
            throw std::logic_error("Plugin registration is only allowed in its own setup");
        }
        return scope;
    }

    bool dependsOn(const Record& consumer, const std::string& provider) const {
        for (const auto& dependency : consumer.spec.dependencies) {
            if (dependency == provider) {
                return true;
            }
            const auto found = records.find(dependency);
            if (found != records.end() && dependsOn(*found->second, provider)) {
                return true;
            }
        }
        return false;
    }

    /** @brief Revoke callbacks and services before running LIFO rollback. */
    static void dispose(Record& record) noexcept {
        record.state = State::inactive;
        record.pinned = false;
        if (record.scope) {
            auto& scope = *record.scope;
            scope.alive = false;
            scope.committed = false;
            scope.listeners.clear();
            for (const auto& entry : scope.services) {
                entry->available = false;
            }
            while (!scope.cleanup.empty()) {
                auto cleanup = std::move(scope.cleanup.back());
                scope.cleanup.pop_back();
                cleanup();
            }
            scope.services.clear();
        }
        record.context.reset();
        record.scope.reset();
    }

    void clearUnchecked() noexcept {
        PhaseGuard guard(*this, Phase::unloading);
        for (auto it = loaded.rbegin(); it != loaded.rend(); ++it) {
            dispose(**it);
        }
        loaded.clear();
    }
};

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}

Runtime::~Runtime() noexcept {
    if (std::this_thread::get_id() != impl_->owner ||
        impl_->phase != Impl::Phase::idle || impl_->borrows != 0) {
        std::terminate();
    }
    impl_->clearUnchecked();
}

void Runtime::add(PluginSpec spec) {
    impl_->checkIdle();
    if (spec.id.empty() || !spec.setup || impl_->records.count(spec.id)) {
        throw std::invalid_argument("Plugin definition requires a unique id and setup");
    }
    std::unordered_set<std::string> dependencies;
    for (const auto& dependency : spec.dependencies) {
        if (dependency.empty() || !dependencies.insert(dependency).second) {
            throw std::invalid_argument("Plugin dependency ids must be nonempty and unique");
        }
    }
    auto record = std::make_unique<Impl::Record>();
    record->spec = std::move(spec);
    const auto id = record->spec.id;
    impl_->records.emplace(id, std::move(record));
}

void Runtime::load(const std::string& id) {
    impl_->checkIdle();
    Impl::PhaseGuard guard(*impl_, Impl::Phase::loading);
    std::vector<Impl::Record*> visited;
    std::vector<Impl::Record*> setupOrder;
    try {
        const auto visit = [&](const auto& recurse, const std::string& current) -> void {
            const auto found = impl_->records.find(current);
            if (found == impl_->records.end()) {
                throw std::logic_error("Missing plugin dependency: " + current);
            }
            auto& record = *found->second;
            if (record.state == Impl::State::active || record.state == Impl::State::staging) {
                return;
            }
            if (record.state == Impl::State::visiting) {
                throw std::logic_error("Plugin dependency cycle: " + current);
            }
            visited.push_back(&record);
            record.state = Impl::State::visiting;
            for (const auto& dependency : record.spec.dependencies) {
                recurse(recurse, dependency);
            }
            setupOrder.push_back(&record);
            record.state = Impl::State::staging;
        };
        visit(visit, id);
        auto* requested = impl_->records.at(id).get();
        for (auto* record : setupOrder) {
            record->scope = std::make_shared<detail::Scope>();
            record->scope->runtime = this;
            record->scope->owner = impl_->owner;
            record->scope->id = record->spec.id;
            record->context.reset(new Context(*this, record->scope));
            impl_->settingUp = record;
            record->spec.setup(*record->context);
            impl_->settingUp = nullptr;
        }
        impl_->loaded.reserve(impl_->loaded.size() + setupOrder.size());
        for (auto* record : setupOrder) {
            record->state = Impl::State::active;
            record->scope->committed = true;
            impl_->loaded.push_back(record);
        }
        requested->pinned = true;
    } catch (...) {
        impl_->settingUp = nullptr;
        impl_->phase = Impl::Phase::unloading;
        for (auto it = setupOrder.rbegin(); it != setupOrder.rend(); ++it) {
            Impl::dispose(**it);
        }
        for (auto* record : visited) {
            record->state = Impl::State::inactive;
        }
        throw;
    }
}

bool Runtime::unload(const std::string& id) {
    impl_->checkIdle();
    const auto found = impl_->records.find(id);
    if (found == impl_->records.end() || found->second->state != Impl::State::active) {
        return false;
    }
    auto* record = found->second.get();
    for (const auto* other : impl_->loaded) {
        if (other != record && impl_->dependsOn(*other, id)) {
            throw std::logic_error("Plugin still has an active dependent: " + other->spec.id);
        }
    }
    Impl::PhaseGuard guard(*impl_, Impl::Phase::unloading);
    Impl::dispose(*record);
    impl_->loaded.erase(std::remove(impl_->loaded.begin(), impl_->loaded.end(), record),
                        impl_->loaded.end());
    for (std::size_t index = impl_->loaded.size(); index > 0; --index) {
        auto* candidate = impl_->loaded[index - 1];
        if (candidate->pinned) {
            continue;
        }
        const bool used = std::any_of(impl_->loaded.begin(), impl_->loaded.end(),
                                     [&](const Impl::Record* other) {
            return other != candidate && impl_->dependsOn(*other, candidate->spec.id);
        });
        if (!used) {
            Impl::dispose(*candidate);
            impl_->loaded.erase(impl_->loaded.begin() + static_cast<std::ptrdiff_t>(index - 1));
        }
    }
    return true;
}

void Runtime::clear() {
    impl_->checkIdle();
    impl_->clearUnchecked();
}

bool Runtime::contains(const std::string& id) const {
    impl_->checkOwner();
    return impl_->records.count(id) != 0;
}

bool Runtime::isLoaded(const std::string& id) const {
    impl_->checkOwner();
    const auto found = impl_->records.find(id);
    return found != impl_->records.end() && found->second->state == Impl::State::active;
}

std::size_t Runtime::loadedCount() const {
    impl_->checkOwner();
    return static_cast<std::size_t>(std::count_if(impl_->loaded.begin(), impl_->loaded.end(),
        [](const Impl::Record* record) { return record->state == Impl::State::active; }));
}

void Runtime::emitErased(std::type_index type, const void* event) {
    impl_->checkIdle();
    Impl::PhaseGuard guard(*impl_, Impl::Phase::emitting);
    for (const auto* record : impl_->loaded) {
        for (const auto& listener : record->scope->listeners) {
            if (listener.first == type) {
                listener.second(event);
            }
        }
    }
}

void Context::rejectEmptyCallback() {
    throw std::invalid_argument("Plugin event listener must be callable");
}

void Context::addListener(std::type_index type,
                          std::function<void(const void*)> callback) {
    auto scope = runtime_.impl_->checkScope(scope_, true);
    scope->listeners.emplace_back(type, std::move(callback));
}

void Context::addService(std::type_index type, std::shared_ptr<void> service) {
    auto scope = runtime_.impl_->checkScope(scope_, true);
    if (!service) {
        throw std::invalid_argument("Plugin service must not be null");
    }
    for (const auto& item : runtime_.impl_->records) {
        if (!item.second->scope) {
            continue;
        }
        for (const auto& entry : item.second->scope->services) {
            if (entry->available && entry->type == type) {
                throw std::logic_error("A live provider already registered this service type");
            }
        }
    }
    auto entry = std::make_shared<detail::ServiceEntry>(
        detail::ServiceEntry{type, std::move(service), scope, true});
    scope->services.push_back(entry);
    try {
        scope->cleanup.emplace_back([entry]() noexcept { entry->value.reset(); });
    } catch (...) {
        scope->services.pop_back();
        throw;
    }
}

std::weak_ptr<detail::ServiceEntry> Context::findService(std::type_index type) {
    auto scope = runtime_.impl_->checkScope(scope_, false);
    const auto& consumer = *runtime_.impl_->records.at(scope->id);
    for (const auto& item : runtime_.impl_->records) {
        const auto& record = *item.second;
        if (!record.scope || !record.scope->alive) {
            continue;
        }
        for (const auto& entry : record.scope->services) {
            if (entry->type != type || !entry->available) {
                continue;
            }
            if (record.spec.id != scope->id &&
                !runtime_.impl_->dependsOn(consumer, record.spec.id)) {
                throw std::logic_error("Service provider is not a declared dependency");
            }
            return entry;
        }
    }
    throw std::logic_error("No loaded dependency provides the requested service");
}

void Context::addCleanup(std::function<void()> cleanup) {
    auto scope = runtime_.impl_->checkScope(scope_, true);
    scope->cleanup.push_back(std::move(cleanup));
}

void detail::borrowService(const std::weak_ptr<Scope>& weakConsumer,
                          const std::weak_ptr<ServiceEntry>& weakEntry,
                          const std::function<void(void*)>& callback) {
    auto consumer = weakConsumer.lock();
    if (!consumer) {
        throw std::logic_error("Service consumer has been unloaded");
    }
    if (std::this_thread::get_id() != consumer->owner) {
        throw std::logic_error("Plugin service borrowed outside its owner thread");
    }
    auto entry = weakEntry.lock();
    auto provider = entry ? entry->provider.lock() : nullptr;
    auto& impl = *consumer->runtime->impl_;
    if (!consumer->alive || !entry || !entry->available || !entry->value ||
        !provider || !provider->alive || impl.phase == Runtime::Impl::Phase::unloading) {
        throw std::logic_error("Plugin service capability has been revoked");
    }
    if ((!consumer->committed || !provider->committed) &&
        (impl.phase != Runtime::Impl::Phase::loading || !impl.settingUp ||
         impl.settingUp->scope != consumer)) {
        throw std::logic_error("Staged services are only visible to the loading plugin");
    }
    ++impl.borrows;
    try {
        callback(entry->value.get());
    } catch (...) {
        --impl.borrows;
        throw;
    }
    --impl.borrows;
}

}
