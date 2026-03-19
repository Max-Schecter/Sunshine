/**
 * @file entry_handler.cpp
 * @brief Definitions for entry handling functions.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <csignal>
#include <filesystem>
#include <format>
#include <iostream>
#include <thread>

// local includes
#include "config.h"
#include "confighttp.h"
#include "entry_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "platform/common.h"
#include "utility.h"

extern "C" {
#ifdef _WIN32
  #include <iphlpapi.h>
#else
  #include <signal.h>
  #include <termios.h>
  #include <unistd.h>
#endif
}

#include <nlohmann/json.hpp>

using namespace std::literals;

namespace {
  size_t curl_write_to_string(const char *ptr, size_t size, size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    const size_t bytes = size * nmemb;
    out->append(ptr, bytes);
    return bytes;
  }

  std::string prompt_hidden(const std::string_view prompt) {
    std::cout << prompt;
    std::cout.flush();

#ifdef _WIN32
    HANDLE stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    if (stdin_handle == INVALID_HANDLE_VALUE || !GetConsoleMode(stdin_handle, &original_mode)) {
      std::string fallback;
      std::getline(std::cin, fallback);
      return fallback;
    }

    DWORD hidden_mode = original_mode & (~ENABLE_ECHO_INPUT);
    SetConsoleMode(stdin_handle, hidden_mode);
    std::string value;
    std::getline(std::cin, value);
    SetConsoleMode(stdin_handle, original_mode);
#else
    struct sigaction old_int {};
    struct sigaction old_term {};
    struct sigaction ignore_action {};
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    ignore_action.sa_flags = 0;

    // Avoid termination while terminal echo is disabled.
    sigaction(SIGINT, &ignore_action, &old_int);
    sigaction(SIGTERM, &ignore_action, &old_term);

    termios old_termios {};
    if (tcgetattr(STDIN_FILENO, &old_termios) != 0) {
      sigaction(SIGINT, &old_int, nullptr);
      sigaction(SIGTERM, &old_term, nullptr);
      std::string fallback;
      std::getline(std::cin, fallback);
      return fallback;
    }

    termios new_termios = old_termios;
    new_termios.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
    std::string value;
    std::getline(std::cin, value);
    tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);

    sigaction(SIGINT, &old_int, nullptr);
    sigaction(SIGTERM, &old_term, nullptr);
#endif

    std::cout << std::endl;
    return value;
  }

  bool pin_is_valid(const std::string &pin) {
    return pin.size() == 4 && std::ranges::all_of(pin, [](const unsigned char ch) {
      return std::isdigit(ch) != 0;
    });
  }
}  // namespace

void launch_ui(const std::optional<std::string> &path) {
  std::string url = std::format("https://localhost:{}", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
  if (path) {
    url += *path;
  }
  platf::open_url(url);
}

namespace args {
  int creds(const char *name, int argc, char *argv[]) {
    if (argc < 2 || argv[0] == "help"sv || argv[1] == "help"sv) {
      help(name);
    }

    http::save_user_creds(config::sunshine.credentials_file, argv[0], argv[1]);

    return 0;
  }

  int help(const char *name) {
    logging::print_help(name);
    return 0;
  }

  int version() {
    // version was already logged at startup
    return 0;
  }

  int pair(const char *name, int argc, char *argv[]) {
    if (argc >= 1 && (argv[0] == "help"sv || argv[0] == "--help"sv || argv[0] == "-h"sv)) {
      std::cout
        << "Usage: "sv << name << " --pair <pin> [device_name] [username]"sv << std::endl
        << "  Submit a pairing PIN to a running local Sunshine instance."sv << std::endl
        << "  The password is prompted securely and is never accepted as a CLI argument."sv << std::endl;
      return 0;
    }
    if (argc < 1 || argc > 3) {
      std::cout << "Usage: "sv << name << " --pair <pin> [device_name] [username]"sv << std::endl;
      return 1;
    }

    const std::string pin = argv[0];
    if (!pin_is_valid(pin)) {
      BOOST_LOG(error) << "PIN must be exactly 4 digits"sv;
      return 1;
    }

    const std::string device_name = argc >= 2 ? argv[1] : "CLI";
    if (device_name.empty()) {
      BOOST_LOG(error) << "Device name must not be empty"sv;
      return 1;
    }

    std::string username = argc >= 3 ? argv[2] : "";
    std::string password;

    if (username.empty()) {
      if (http::reload_user_creds(config::sunshine.credentials_file) == 0 && !config::sunshine.username.empty()) {
        username = config::sunshine.username;
      }

      if (username.empty()) {
        std::cout << "Web UI username: "sv;
        std::getline(std::cin, username);
      } else {
        std::cout << "Web UI username ["sv << username << "]: ";
        std::string entered_username;
        std::getline(std::cin, entered_username);
        if (!entered_username.empty()) {
          username = std::move(entered_username);
        }
      }
    }

    password = prompt_hidden("Web UI password: "sv);

    if (username.empty() || password.empty()) {
      BOOST_LOG(error) << "Username and password are required for --pair"sv;
      return 1;
    }
    auto clear_password_guard = util::fail_guard([&password]() {
      std::ranges::fill(password, '\0');
      password.clear();
    });

    CURL *curl = curl_easy_init();
    if (!curl) {
      BOOST_LOG(error) << "Failed to initialize curl"sv;
      return 1;
    }

    std::string response_body;
    auto *headers = curl_slist_append(nullptr, "Content-Type: application/json");
    auto cleanup = util::fail_guard([&]() {
      if (headers) {
        curl_slist_free_all(headers);
      }
      curl_easy_cleanup(curl);
    });
    if (!headers) {
      BOOST_LOG(error) << "Failed to initialize request headers"sv;
      return 1;
    }

    nlohmann::json payload;
    payload["pin"] = pin;
    payload["name"] = device_name;
    const std::string payload_str = payload.dump();

    const std::string cert_path = config::nvhttp.cert;
    if (cert_path.empty() || !std::filesystem::exists(cert_path)) {
      BOOST_LOG(error) << "Sunshine certificate not found: "sv << cert_path;
      BOOST_LOG(info) << "Start Sunshine once so credentials/certificates are generated."sv;
      return 1;
    }

    const std::string url = std::format("https://localhost:{}/api/pin", static_cast<int>(net::map_port(confighttp::PORT_HTTPS)));
    auto setopt = [curl](CURLoption option, auto value) {
      return curl_easy_setopt(curl, option, value) == CURLE_OK;
    };

    if (!setopt(CURLOPT_URL, url.c_str()) ||
        !setopt(CURLOPT_HTTPAUTH, CURLAUTH_BASIC) ||
        !setopt(CURLOPT_USERNAME, username.c_str()) ||
        !setopt(CURLOPT_PASSWORD, password.c_str()) ||
        !setopt(CURLOPT_HTTPHEADER, headers) ||
        !setopt(CURLOPT_POST, 1L) ||
        !setopt(CURLOPT_POSTFIELDS, payload_str.c_str()) ||
        !setopt(CURLOPT_POSTFIELDSIZE, static_cast<long>(payload_str.size())) ||
        !setopt(CURLOPT_WRITEFUNCTION, curl_write_to_string) ||
        !setopt(CURLOPT_WRITEDATA, &response_body) ||
        !setopt(CURLOPT_CONNECTTIMEOUT, 5L) ||
        !setopt(CURLOPT_TIMEOUT, 10L) ||
        !setopt(CURLOPT_FOLLOWLOCATION, 1L) ||
        !setopt(CURLOPT_MAXREDIRS, 3L) ||
        !setopt(CURLOPT_SSL_VERIFYPEER, 1L) ||
        // Sunshine's self-signed cert currently uses CN without localhost SANs.
        // We verify against the exact cert via CAINFO and disable hostname checks.
        !setopt(CURLOPT_SSL_VERIFYHOST, 0L) ||
        !setopt(CURLOPT_CAINFO, cert_path.c_str())) {
      BOOST_LOG(error) << "Failed to configure curl request"sv;
      return 1;
    }

    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
      BOOST_LOG(error) << "Failed to reach Sunshine API: "sv << curl_easy_strerror(result);
      BOOST_LOG(info) << "Ensure Sunshine is running and the Web UI is reachable on localhost"sv;
      return 1;
    }

    long status_code = 0;
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code) != CURLE_OK) {
      BOOST_LOG(error) << "Failed to read HTTP response code"sv;
      return 1;
    }
    char *effective_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);

    if (effective_url != nullptr && std::string_view {effective_url}.find("/welcome") != std::string_view::npos) {
      BOOST_LOG(error) << "Web UI credentials are not configured yet. Set them before using --pair."sv;
      return 1;
    }

    if (status_code == 401) {
      BOOST_LOG(error) << "Unauthorized: invalid Web UI credentials"sv;
      return 1;
    }
    if (status_code == 403) {
      BOOST_LOG(error) << "Forbidden: this client is not allowed by origin_web_ui_allowed"sv;
      return 1;
    }

    try {
      const auto json = nlohmann::json::parse(response_body.empty() ? "{}"s : response_body);
      if (status_code == 200 && json.value("status", false)) {
        BOOST_LOG(info) << "Pairing PIN submitted successfully"sv;
        return 0;
      }

      const auto error_message = json.value("error", std::string("Pairing failed"));
      BOOST_LOG(error) << error_message;
      return 1;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Unexpected response from Sunshine API (HTTP "sv << status_code << "): "sv << e.what();
      return 1;
    }
  }

#ifdef _WIN32
  int restore_nvprefs_undo() {
    if (nvprefs_instance.load()) {
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      nvprefs_instance.unload();
    }
    return 0;
  }
#endif
}  // namespace args

namespace lifetime {
  char **argv;
  std::atomic_int desired_exit_code;

  void exit_sunshine(int exit_code, bool async) {
    // Store the exit code of the first exit_sunshine() call
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    // Raise SIGINT to start termination
    std::raise(SIGINT);

    // Termination will happen asynchronously, but the caller may
    // have wanted synchronous behavior.
    while (!async) {
      std::this_thread::sleep_for(1s);
    }
  }

  void debug_trap() {
#ifdef _WIN32
    DebugBreak();
#else
    std::raise(SIGTRAP);
#endif
  }

  char **get_argv() {
    return argv;
  }
}  // namespace lifetime

void log_publisher_data() {
  BOOST_LOG(info) << "Package Publisher: "sv << SUNSHINE_PUBLISHER_NAME;
  BOOST_LOG(info) << "Publisher Website: "sv << SUNSHINE_PUBLISHER_WEBSITE;
  BOOST_LOG(info) << "Get support: "sv << SUNSHINE_PUBLISHER_ISSUE_URL;
}

#ifdef _WIN32
bool is_gamestream_enabled() {
  DWORD enabled;
  DWORD size = sizeof(enabled);
  return RegGetValueW(
           HKEY_LOCAL_MACHINE,
           L"SOFTWARE\\NVIDIA Corporation\\NvStream",
           L"EnableStreaming",
           RRF_RT_REG_DWORD,
           nullptr,
           &enabled,
           &size
         ) == ERROR_SUCCESS &&
         enabled != 0;
}

namespace service_ctrl {
  class service_controller {
  public:
    /**
     * @brief Constructor for service_controller class.
     * @param service_desired_access SERVICE_* desired access flags.
     */
    service_controller(DWORD service_desired_access) {
      scm_handle = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
      if (!scm_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenSCManager() failed: "sv << winerr;
        return;
      }

      service_handle = OpenServiceA(scm_handle, "SunshineService", service_desired_access);
      if (!service_handle) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "OpenService() failed: "sv << winerr;
        return;
      }
    }

    ~service_controller() {
      if (service_handle) {
        CloseServiceHandle(service_handle);
      }

      if (scm_handle) {
        CloseServiceHandle(scm_handle);
      }
    }

    /**
     * @brief Asynchronously starts the Sunshine service.
     */
    bool start_service() {
      if (!service_handle) {
        return false;
      }

      if (!StartServiceA(service_handle, 0, nullptr)) {
        auto winerr = GetLastError();
        if (winerr != ERROR_SERVICE_ALREADY_RUNNING) {
          BOOST_LOG(error) << "StartService() failed: "sv << winerr;
          return false;
        }
      }

      return true;
    }

    /**
     * @brief Query the service status.
     * @param status The SERVICE_STATUS struct to populate.
     */
    bool query_service_status(SERVICE_STATUS &status) {
      if (!service_handle) {
        return false;
      }

      if (!QueryServiceStatus(service_handle, &status)) {
        auto winerr = GetLastError();
        BOOST_LOG(error) << "QueryServiceStatus() failed: "sv << winerr;
        return false;
      }

      return true;
    }

  private:
    SC_HANDLE scm_handle = nullptr;
    SC_HANDLE service_handle = nullptr;
  };

  bool is_service_running() {
    service_controller sc {SERVICE_QUERY_STATUS};

    SERVICE_STATUS status;
    if (!sc.query_service_status(status)) {
      return false;
    }

    return status.dwCurrentState == SERVICE_RUNNING;
  }

  bool start_service() {
    service_controller sc {SERVICE_QUERY_STATUS | SERVICE_START};

    std::cout << "Starting Sunshine..."sv;

    // This operation is asynchronous, so we must wait for it to complete
    if (!sc.start_service()) {
      return false;
    }

    SERVICE_STATUS status;
    do {
      Sleep(1000);
      std::cout << '.';
    } while (sc.query_service_status(status) && status.dwCurrentState == SERVICE_START_PENDING);

    if (status.dwCurrentState != SERVICE_RUNNING) {
      BOOST_LOG(error) << std::format("{} failed to start: {}"sv, platf::SERVICE_NAME, status.dwWin32ExitCode);
      return false;
    }

    std::cout << std::endl;
    return true;
  }

  bool wait_for_ui_ready() {
    std::cout << "Waiting for Web UI to be ready...";

    // Wait up to 30 seconds for the web UI to start
    for (int i = 0; i < 30; i++) {
      PMIB_TCPTABLE tcp_table = nullptr;
      ULONG table_size = 0;
      ULONG err;

      auto fg = util::fail_guard([&tcp_table]() {
        free(tcp_table);
      });

      do {
        // Query all open TCP sockets to look for our web UI port
        err = GetTcpTable(tcp_table, &table_size, false);
        if (err == ERROR_INSUFFICIENT_BUFFER) {
          free(tcp_table);
          tcp_table = (PMIB_TCPTABLE) malloc(table_size);
        }
      } while (err == ERROR_INSUFFICIENT_BUFFER);

      if (err != NO_ERROR) {
        BOOST_LOG(error) << "Failed to query TCP table: "sv << err;
        return false;
      }

      uint16_t port_nbo = htons(net::map_port(confighttp::PORT_HTTPS));
      for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
        auto &entry = tcp_table->table[i];

        // Look for our port in the listening state
        if (entry.dwLocalPort == port_nbo && entry.dwState == MIB_TCP_STATE_LISTEN) {
          std::cout << std::endl;
          return true;
        }
      }

      Sleep(1000);
      std::cout << '.';
    }

    std::cout << "timed out"sv << std::endl;
    return false;
  }
}  // namespace service_ctrl
#endif
