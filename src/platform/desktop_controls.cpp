#include "platform/desktop_controls.h"
#include "log.h"
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#include <roapi.h>
#include <shellapi.h>
#include <SystemMediaTransportControlsInterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
using namespace winrt;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Storage::Streams;
#endif

namespace ap::platform {
struct DesktopControls::Impl {
#if defined(_WIN32)
    struct Commands {
        std::mutex mutex;
        std::function<void(airplay::MediaCommand)> send;
        bool enabled = false, playing = false;
    };
    std::shared_ptr<Commands> commands = std::make_shared<Commands>();
    SystemMediaTransportControls media{nullptr};
    event_token button{};
    bool subscribed = false, apartment = SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED));
    HWND window = nullptr, owner = nullptr;
    NOTIFYICONDATAW icon{};
    bool tray = false, active = false, playing = false, controls = false;
    uint32_t elapsed = UINT32_MAX, total = UINT32_MAX;
    std::function<void(bool)> visibility;
    std::function<void()> quit;
    static constexpr UINT callback = WM_APP + 73;
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, w, l);
        if (message == RegisterWindowMessageW(L"TaskbarCreated")) {
            self->tray = Shell_NotifyIconW(NIM_ADD, &self->icon) != FALSE;
            return 0;
        }
        if (message == callback) {
            if (l == WM_LBUTTONDBLCLK) self->visibility(true);
            if (l == WM_RBUTTONUP) {
                HMENU menu = CreatePopupMenu();
                if (!menu) return 0;
                AppendMenuW(menu, MF_STRING, 1, L"Show");
                AppendMenuW(menu, MF_STRING, 2, L"Hide");
                AppendMenuW(menu, MF_STRING, 3, L"Exit");
                POINT point{}; GetCursorPos(&point);
                SetForegroundWindow(hwnd);
                const auto choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                    point.x, point.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                if (choice) SendMessageW(hwnd, WM_COMMAND, choice, 0);
            }
            return 0;
        }
        if (message == WM_COMMAND) {
            if (LOWORD(w) == 1 || LOWORD(w) == 2) self->visibility(LOWORD(w) == 1);
            if (LOWORD(w) == 3) self->quit();
            return 0;
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
#endif
};
DesktopControls::DesktopControls() : impl_(std::make_unique<Impl>()) {}
DesktopControls::~DesktopControls() {
    stop();
#if defined(_WIN32)
    if (impl_->apartment) RoUninitialize();
#endif
}
bool DesktopControls::start(void* native, std::function<void(bool)> visibility,
                            std::function<void()> quit,
                            std::function<void(airplay::MediaCommand)> command) {
#if defined(_WIN32)
    auto& s = *impl_;
    s.owner = static_cast<HWND>(native);
    s.visibility = std::move(visibility); s.quit = std::move(quit);
    WNDCLASSW cls{};
    cls.lpfnWndProc = Impl::procedure; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"AirPlayWindows.DesktopControls";
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    s.window = CreateWindowExW(0, cls.lpszClassName, L"AirPlay-Windows", 0,
                               0, 0, 0, 0, nullptr, nullptr, cls.hInstance, &s);
    if (!s.window) return false;
    s.icon.cbSize = sizeof(s.icon); s.icon.hWnd = s.window; s.icon.uID = 1;
    s.icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; s.icon.uCallbackMessage = Impl::callback;
    s.icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wcscpy_s(s.icon.szTip, L"AirPlay-Windows");
    s.tray = Shell_NotifyIconW(NIM_ADD, &s.icon) != FALSE;
    try {
        auto interop = get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
        check_hresult(interop->GetForWindow(s.owner, guid_of<SystemMediaTransportControls>(), put_abi(s.media)));
        auto state = s.commands;
        state->send = std::move(command);
        s.button = s.media.ButtonPressed([state](auto&&, const SystemMediaTransportControlsButtonPressedEventArgs& args) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->enabled || !state->send) return;
            const auto button = args.Button();
            using Button = SystemMediaTransportControlsButton;
            if (button == Button::Play && state->playing) return;
            if (button == Button::Pause && !state->playing) return;
            if (button == Button::Play || button == Button::Pause)
                state->send(airplay::MediaCommand::PlayPause);
            else if (button == Button::Next) state->send(airplay::MediaCommand::Next);
            else if (button == Button::Previous) state->send(airplay::MediaCommand::Previous);
        });
        s.subscribed = true;
        s.media.IsEnabled(false);
        metadata("AirPlay-Windows", "", "");
        LOG_INFO << "Windows media controls ready (DACP)";
    } catch (const hresult_error& e) { LOG_WARN << "Windows media controls: " << to_string(e.message()); }
    return s.tray;
#else
    (void)native; (void)visibility; (void)quit; (void)command;
    return false;
#endif
}
void DesktopControls::stop() {
#if defined(_WIN32)
    auto& s = *impl_;
    { std::lock_guard<std::mutex> lock(s.commands->mutex); s.commands->enabled = false; s.commands->send = {}; }
    try {
        if (s.media) {
            if (s.subscribed) s.media.ButtonPressed(s.button);
            s.media.IsEnabled(false); s.media = nullptr;
        }
    } catch (const hresult_error& e) { LOG_WARN << "Windows media shutdown: " << to_string(e.message()); }
    s.subscribed = false;
    if (s.tray) Shell_NotifyIconW(NIM_DELETE, &s.icon);
    s.tray = false;
    if (s.window) DestroyWindow(s.window);
    s.window = nullptr;
#endif
}
void DesktopControls::metadata(const std::string& title, const std::string& artist, const std::string& album) {
#if defined(_WIN32)
    try { if (impl_->media) {
        auto updater = impl_->media.DisplayUpdater(); updater.Type(MediaPlaybackType::Music);
        auto music = updater.MusicProperties(); music.Title(to_hstring(title.empty() ? std::string("AirPlay-Windows") : title));
        music.Artist(to_hstring(artist)); music.AlbumTitle(to_hstring(album)); updater.Update();
    }} catch (const hresult_error& e) { LOG_WARN << "Windows media metadata: " << to_string(e.message()); }
#else
    (void)title; (void)artist; (void)album;
#endif
}
void DesktopControls::artwork(const uint8_t* data, std::size_t size) {
#if defined(_WIN32)
    try { if (impl_->media) {
        auto updater = impl_->media.DisplayUpdater();
        if (size) {
            InMemoryRandomAccessStream stream;
            DataWriter writer(stream); writer.WriteBytes(array_view<const uint8_t>(data, data + size));
            writer.StoreAsync().get(); writer.DetachStream(); stream.Seek(0);
            updater.Thumbnail(RandomAccessStreamReference::CreateFromStream(stream));
        } else updater.Thumbnail(nullptr);
        updater.Update();
    }} catch (const hresult_error& e) { LOG_WARN << "Windows media artwork: " << to_string(e.message()); }
#else
    (void)data; (void)size;
#endif
}
void DesktopControls::publish(bool active, bool playing, bool controls, uint32_t elapsed, uint32_t total) {
#if defined(_WIN32)
    auto& s = *impl_;
    { std::lock_guard<std::mutex> lock(s.commands->mutex); s.commands->enabled = active && controls; s.commands->playing = playing; }
    try { if (s.media) {
        if (active != s.active || playing != s.playing || controls != s.controls) {
            s.media.IsEnabled(active); s.media.IsPlayEnabled(controls); s.media.IsPauseEnabled(controls);
            s.media.IsNextEnabled(controls); s.media.IsPreviousEnabled(controls);
            s.media.PlaybackStatus(!active ? MediaPlaybackStatus::Closed : playing ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused);
            s.active = active; s.playing = playing; s.controls = controls;
        }
        if (elapsed / 1000 != s.elapsed / 1000 || total != s.total) {
            SystemMediaTransportControlsTimelineProperties timeline;
            timeline.StartTime(winrt::Windows::Foundation::TimeSpan{0}); timeline.MinSeekTime(winrt::Windows::Foundation::TimeSpan{0});
            timeline.EndTime(winrt::Windows::Foundation::TimeSpan{static_cast<int64_t>(total) * 10000});
            timeline.MaxSeekTime(timeline.EndTime());
            timeline.Position(winrt::Windows::Foundation::TimeSpan{static_cast<int64_t>(elapsed > total ? total : elapsed) * 10000});
            s.media.UpdateTimelineProperties(timeline); s.elapsed = elapsed; s.total = total;
        }
    }} catch (const hresult_error& e) { LOG_WARN << "Windows media state: " << to_string(e.message()); }
#else
    (void)active; (void)playing; (void)controls; (void)elapsed; (void)total;
#endif
}
}
