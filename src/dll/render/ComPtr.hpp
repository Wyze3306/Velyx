#pragma once

#include <utility>

namespace velyx {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}

    explicit ComPtr(T* pointer, bool addRef = true) : pointer_(pointer) {
        if (pointer_ && addRef) pointer_->AddRef();
    }

    ComPtr(const ComPtr& other) : pointer_(other.pointer_) {
        if (pointer_) pointer_->AddRef();
    }

    ComPtr(ComPtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}

    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& other) {
        if (this != std::addressof(other)) {
            reset();
            pointer_ = other.pointer_;
            if (pointer_) pointer_->AddRef();
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != std::addressof(other)) {
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
        }
        return *this;
    }

    void reset() {
        if (pointer_) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

    void attach(T* pointer) {
        reset();
        pointer_ = pointer;
    }

    [[nodiscard]] T* detach() { return std::exchange(pointer_, nullptr); }

    [[nodiscard]] T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    explicit operator bool() const { return pointer_ != nullptr; }
    bool operator==(std::nullptr_t) const { return pointer_ == nullptr; }

    T** put() {
        reset();
        return std::addressof(pointer_);
    }

    template <typename U>
    HRESULT as(ComPtr<U>& target) const {
        if (!pointer_) return E_POINTER;
        return pointer_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(target.put()));
    }

private:
    T* pointer_ = nullptr;
};

}
