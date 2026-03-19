#include "clipboard.h"

#include "src/logging.h"
#include "utf_utils.h"

#include <chrono>
#include <cstring>
#include <optional>
#include <mutex>
#include <thread>

using namespace std::literals;

namespace platf::clipboard {
  namespace {
    constexpr UINT WM_SUNSHINE_APPLY_CLIPBOARD = WM_APP + 52;

    HWND g_hwnd = nullptr;
    std::mutex g_mutex;
    std::optional<update_t> g_pending_local;
    struct remote_update_t {
      update_t update;
      std::uint32_t observed_local_generation;
    };
    std::optional<remote_update_t> g_pending_remote;
    std::string g_last_remote_text;
    std::uint32_t g_generation = 0;
    bool g_available = false;

    bool read_clipboard_text(std::string &text) {
      if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        text.clear();
        return true;
      }

      if (!OpenClipboard(g_hwnd)) {
        BOOST_LOG(debug) << "Clipboard read failed: OpenClipboard() failed"sv;
        return false;
      }

      HANDLE handle = GetClipboardData(CF_UNICODETEXT);
      if (handle == nullptr) {
        BOOST_LOG(debug) << "Clipboard read failed: GetClipboardData() returned null"sv;
        CloseClipboard();
        return false;
      }

      auto *wide = static_cast<wchar_t *>(GlobalLock(handle));
      if (wide == nullptr) {
        BOOST_LOG(debug) << "Clipboard read failed: GlobalLock() returned null"sv;
        CloseClipboard();
        return false;
      }

      text = utf_utils::to_utf8(wide);
      if (!GlobalUnlock(handle) && GetLastError() != NO_ERROR) {
        BOOST_LOG(debug) << "Clipboard read warning: GlobalUnlock() reported an error"sv;
      }
      CloseClipboard();
      return true;
    }

    bool write_clipboard_text(const std::string &text) {
      std::wstring wide = utf_utils::from_utf8(text);
      SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);

      for (int attempt = 0; attempt < 5; attempt++) {
        if (!OpenClipboard(g_hwnd)) {
          BOOST_LOG(debug) << "Clipboard write retry: OpenClipboard() failed"sv;
          std::this_thread::sleep_for((5 + attempt * 5) * 1ms);
          continue;
        }

        EmptyClipboard();

        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (handle == nullptr) {
          BOOST_LOG(debug) << "Clipboard write failed: GlobalAlloc() returned null"sv;
          CloseClipboard();
          return false;
        }

        void *dest = GlobalLock(handle);
        if (dest == nullptr) {
          BOOST_LOG(debug) << "Clipboard write failed: GlobalLock() returned null"sv;
          GlobalFree(handle);
          CloseClipboard();
          return false;
        }

        memcpy(dest, wide.c_str(), bytes);
        if (!GlobalUnlock(handle) && GetLastError() != NO_ERROR) {
          BOOST_LOG(debug) << "Clipboard write warning: GlobalUnlock() reported an error"sv;
        }

        if (SetClipboardData(CF_UNICODETEXT, handle) == nullptr) {
          BOOST_LOG(debug) << "Clipboard write retry: SetClipboardData() failed"sv;
          GlobalFree(handle);
          CloseClipboard();
          std::this_thread::sleep_for((5 + attempt * 5) * 1ms);
          continue;
        }

        CloseClipboard();
        return true;
      }

      return false;
    }

    void handle_local_clipboard_change() {
      std::string text;
      if (!read_clipboard_text(text)) {
        return;
      }

      std::lock_guard<std::mutex> lock(g_mutex);
      if (text == g_last_remote_text) {
        return;
      }

      g_pending_local = update_t {std::move(text), ++g_generation};
    }

    void apply_remote_clipboard_change() {
      std::optional<remote_update_t> remote_update;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        remote_update.swap(g_pending_remote);
      }

      if (!remote_update) {
        return;
      }

      {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (remote_update->observed_local_generation != g_generation) {
          BOOST_LOG(debug) << "Dropping stale remote clipboard update after newer local change"sv;
          return;
        }
      }

      if (!write_clipboard_text(remote_update->update.text)) {
        BOOST_LOG(debug) << "Clipboard write timed out or failed"sv;
        return;
      }

      std::lock_guard<std::mutex> lock(g_mutex);
      g_last_remote_text = remote_update->update.text;
    }
  }  // namespace

  bool init(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_hwnd = hwnd;
    g_available = AddClipboardFormatListener(hwnd);
    if (!g_available) {
      BOOST_LOG(debug) << "Clipboard listener initialization failed"sv;
    }
    return g_available;
  }

  void deinit() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_hwnd != nullptr) {
      RemoveClipboardFormatListener(g_hwnd);
    }
    g_available = false;
    g_hwnd = nullptr;
    g_pending_local.reset();
    g_pending_remote.reset();
  }

  bool handle_window_message(HWND hwnd, UINT uMsg, WPARAM, LPARAM) {
    if (uMsg == WM_CLIPBOARDUPDATE) {
      handle_local_clipboard_change();
      return true;
    }

    if (uMsg == WM_SUNSHINE_APPLY_CLIPBOARD) {
      apply_remote_clipboard_change();
      return true;
    }

    return false;
  }

  std::optional<update_t> consume_pending_local_update() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::optional<update_t> update;
    update.swap(g_pending_local);
    return update;
  }

  void enqueue_remote_update(std::string text, std::uint32_t generation) {
    HWND hwnd = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_pending_remote = remote_update_t {
        update_t {std::move(text), generation},
        g_generation
      };
      hwnd = g_hwnd;
    }

    if (hwnd != nullptr) {
      PostMessage(hwnd, WM_SUNSHINE_APPLY_CLIPBOARD, 0, 0);
    }
  }

  bool is_available() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_available;
  }
}
