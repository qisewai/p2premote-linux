#pragma once

#include <string>

class QWidget;

class SingleInstance final {
public:
    enum class Result { primary, activated, failed };

    SingleInstance();
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    Result start(bool wait_for_exit);
    bool registerWindow(QWidget& window);
    void unregisterWindow();
    static bool isActivationMessage(unsigned message);

private:
    const std::wstring name_;
    void* mutex_{};
    void* window_{};
};
