#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dll/hook/Hook.hpp"

namespace velyx {

class HookManager {
public:
    static HookManager& get();

    template <typename T, typename... Args>
    T* add(Args&&... args) {
        auto hook = std::make_unique<T>(std::forward<Args>(args)...);
        T* raw = hook.get();
        hooks_.push_back(std::move(hook));
        return raw;
    }

    bool installAll();

    void uninstallAll();

    [[nodiscard]] std::vector<std::string> failed() const;
    [[nodiscard]] size_t count() const { return hooks_.size(); }
    [[nodiscard]] bool ready() const { return initialised_; }

private:
    HookManager() = default;

    std::vector<std::unique_ptr<Hook>> hooks_;
    bool initialised_ = false;
};

}
