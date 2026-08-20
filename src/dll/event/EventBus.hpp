#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace velyx {

struct Event {
    virtual ~Event() = default;
};

struct Cancellable : Event {
    bool cancelled = false;

    void cancel() { cancelled = true; }
    [[nodiscard]] bool isCancelled() const { return cancelled; }
};

enum class EventPriority : uint8_t {
    First = 0,
    High = 1,
    Normal = 2,
    Low = 3,
    Last = 4,
};

class EventBus {
public:
    using Handle = uint64_t;

    static EventBus& get() {
        static EventBus instance;
        return instance;
    }

    template <typename E>
    Handle on(std::function<void(E&)> handler, EventPriority priority = EventPriority::Normal,
              const void* owner = nullptr) {
        static_assert(std::is_base_of_v<Event, E>, "events must derive from velyx::Event");

        Entry entry;
        entry.id = ++nextHandle_;
        entry.priority = priority;
        entry.owner = owner;
        entry.invoke = [handler = std::move(handler)](Event& event) {
            handler(static_cast<E&>(event));
        };

        const std::type_index type(typeid(E));

        const std::lock_guard lock(mutex_);
        if (emitting_ > 0) {
            pendingAdds_.push_back({type, std::move(entry)});
        } else {
            insert(type, std::move(entry));
        }

        return nextHandle_;
    }

    template <typename E, typename T>
    Handle on(T* instance, void (T::*method)(E&),
              EventPriority priority = EventPriority::Normal) {
        return on<E>([instance, method](E& event) { (instance->*method)(event); }, priority,
                     instance);
    }

    template <typename E>
    void emit(E& event) {
        static_assert(std::is_base_of_v<Event, E>, "events must derive from velyx::Event");

        const std::type_index type(typeid(E));

        std::vector<Entry>* entries = nullptr;
        {
            const std::lock_guard lock(mutex_);
            const auto it = listeners_.find(type);
            if (it == listeners_.end() || it->second.empty()) return;
            entries = &it->second;
            ++emitting_;
        }

        // Les handlers tournent sans le verrou : un module peut rappeler le bus,
        // et le tenir pendant du code du jeu invite au blocage.
        for (const auto& entry : *entries) {
            if constexpr (std::is_base_of_v<Cancellable, E>) {

                if (event.cancelled && entry.priority != EventPriority::Last) continue;
            }
            entry.invoke(event);
        }

        const std::lock_guard lock(mutex_);
        if (--emitting_ == 0) flushPending();
    }

    void off(Handle handle) {
        const std::lock_guard lock(mutex_);
        if (emitting_ > 0) {
            pendingRemovals_.push_back(handle);
            return;
        }
        removeHandle(handle);
    }

    void offOwner(const void* owner) {
        const std::lock_guard lock(mutex_);
        if (emitting_ > 0) {
            pendingOwnerRemovals_.push_back(owner);
            return;
        }
        removeOwner(owner);
    }

    void clear() {
        const std::lock_guard lock(mutex_);
        listeners_.clear();
        pendingAdds_.clear();
        pendingRemovals_.clear();
        pendingOwnerRemovals_.clear();
    }

    [[nodiscard]] size_t listenerCount() const {
        const std::lock_guard lock(mutex_);
        size_t total = 0;
        for (const auto& [type, entries] : listeners_) total += entries.size();
        return total;
    }

private:
    struct Entry {
        Handle id = 0;
        EventPriority priority = EventPriority::Normal;
        const void* owner = nullptr;
        std::function<void(Event&)> invoke;
    };

    void insert(const std::type_index& type, Entry entry) {
        auto& entries = listeners_[type];
        const auto position = std::ranges::upper_bound(
            entries, entry.priority, {}, [](const Entry& e) { return e.priority; });
        entries.insert(position, std::move(entry));
    }

    void removeHandle(Handle handle) {
        for (auto& [type, entries] : listeners_) {
            std::erase_if(entries, [handle](const Entry& e) { return e.id == handle; });
        }
    }

    void removeOwner(const void* owner) {
        for (auto& [type, entries] : listeners_) {
            std::erase_if(entries, [owner](const Entry& e) { return e.owner == owner; });
        }
    }

    void flushPending() {
        for (auto& [type, entry] : pendingAdds_) insert(type, std::move(entry));
        pendingAdds_.clear();

        for (const Handle handle : pendingRemovals_) removeHandle(handle);
        pendingRemovals_.clear();

        for (const void* owner : pendingOwnerRemovals_) removeOwner(owner);
        pendingOwnerRemovals_.clear();
    }

    mutable std::recursive_mutex mutex_;
    std::unordered_map<std::type_index, std::vector<Entry>> listeners_;

    int emitting_ = 0;
    std::vector<std::pair<std::type_index, Entry>> pendingAdds_;
    std::vector<Handle> pendingRemovals_;
    std::vector<const void*> pendingOwnerRemovals_;

    Handle nextHandle_ = 0;
};

inline EventBus& events() { return EventBus::get(); }

}
