#pragma once

#include <mutex>
#include <winrt/base.h>

namespace p2premote {

class WinrtApartment {
public:
    explicit WinrtApartment(winrt::apartment_type type = winrt::apartment_type::multi_threaded) {
        winrt::init_apartment(type);
        const std::lock_guard lock(mutex_);
        ++active_;
    }

    ~WinrtApartment() {
        {
            const std::lock_guard lock(mutex_);
            // Clear cached factories while the last participating COM apartment is still alive.
            if (--active_ == 0) winrt::clear_factory_cache();
        }
        winrt::uninit_apartment();
    }

    WinrtApartment(const WinrtApartment&) = delete;
    WinrtApartment& operator=(const WinrtApartment&) = delete;

private:
    inline static std::mutex mutex_;
    inline static unsigned active_{};
};

} // namespace p2premote
