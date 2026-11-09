#pragma once

#if defined(_WIN32)
#include <windows.h>

namespace ap::platform {

inline constexpr wchar_t kInstanceWindowProperty[] = L"AirPlay-Windows.MainWindow.v1";

// Session-local kernel objects disappear automatically after the last process
// closes them, including after a crash. The event also remembers an activation
// requested while the first process is still creating its window.
class SingleInstance {
public:
    enum class Result { Primary, Secondary, Error };

    Result acquire(bool activate = true) {
        event_ = CreateEventW(nullptr, FALSE, FALSE,
                              L"Local\\AirPlay-Windows.Activate.v1");
        if (!event_) return Result::Error;
        mutex_ = CreateMutexW(nullptr, FALSE,
                              L"Local\\AirPlay-Windows.Instance.v1");
        const DWORD error = GetLastError();
        if (!mutex_) return Result::Error;
        if (error != ERROR_ALREADY_EXISTS) return Result::Primary;

        if (!activate) return Result::Secondary;
        // Give only this application's window permission to take foreground.
        EnumWindows([](HWND window, LPARAM) -> BOOL {
            if (!GetPropW(window, kInstanceWindowProperty)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            AllowSetForegroundWindow(pid);
            return FALSE;
        }, 0);
        return SetEvent(event_) ? Result::Secondary : Result::Error;
    }

    bool activation_requested() const {
        return event_ && WaitForSingleObject(event_, 0) == WAIT_OBJECT_0;
    }

    ~SingleInstance() {
        if (mutex_) CloseHandle(mutex_);
        if (event_) CloseHandle(event_);
    }
    SingleInstance() = default;
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

private:
    HANDLE mutex_ = nullptr;
    HANDLE event_ = nullptr;
};

} // namespace ap::platform
#endif
