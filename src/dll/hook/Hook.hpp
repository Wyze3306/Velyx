#pragma once

#include <cstdint>
#include <string>

namespace velyx {

class Hook {
public:
    Hook(std::string name, uintptr_t target);
    virtual ~Hook();

    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;

    virtual bool install() = 0;

    virtual void uninstall();

    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] uintptr_t target() const { return target_; }
    [[nodiscard]] bool installed() const { return installed_; }

protected:

    bool create(void* detour, void** original);

    bool createAt(void* address, void* detour, void** original);

    std::string name_;
    uintptr_t target_ = 0;
    void* hooked_ = nullptr;
    bool installed_ = false;
};

}
