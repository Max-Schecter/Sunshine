#pragma once

#include <Windows.h>

extern "C" {
#include <moonlight-common-c/src/Input.h>
}

#include <cstdint>
#include <optional>
#include <string>

namespace platf::clipboard {
  constexpr std::size_t max_clipboard_text_length = SS_CLIPBOARD_TEXT_MAX_LENGTH;

  struct update_t {
    std::string text;
    std::uint32_t generation;
  };

  bool init(HWND hwnd);
  void deinit();
  bool handle_window_message(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  std::optional<update_t> consume_pending_local_update();
  void enqueue_remote_update(std::string text, std::uint32_t generation);
  bool is_available();
}
