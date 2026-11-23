#pragma once

#include <p2premote/core_api.h>
#include "account_api.h"

#include <windows.h>
#include <winrt/base.h>

#include <stdexcept>
#include <utility>

namespace p2premote::abi {

void SetError(const wchar_t* message) noexcept;
int32_t ReportException() noexcept;

template<class F>
int32_t Guard(F&& operation) noexcept {
    try {
        std::forward<F>(operation)();
        return P2P_OK;
    } catch (...) {
        return ReportException();
    }
}

template<class T>
T& Required(T* value) {
    if (!value) throw std::invalid_argument("缺少核心接口参数");
    return *value;
}

struct Apartment {
    explicit Apartment(winrt::apartment_type type);
    ~Apartment();
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};

p2premote::ConnectionOptions GetConnection(const p2p_account* account);

} // namespace p2premote::abi
