#include "podcast_catalog.h"
#include "podcast_config.h"
#include "podcast_player.h"
#include "podcast_ui.h"
#include "radio_display_settings.h"
#include "radio_sleep_timer.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

constexpr char kTag[] = "podcast_main";
constexpr std::size_t kEpisodeCapacity = 8;
constexpr std::size_t kMaxVisibleNetworks = 12;

struct KeyMessage {
    bsp_btn_t button;
    bsp_btn_ev_t event;
};

enum class ConfigInputMode {
    NetworkList,
    Keyboard,
    Connecting,
};

enum class SettingsInputMode {
    Menu,
    Volume,
    SleepTimer,
    Display,
    WifiMenu,
    SavedWifi,
    WifiAction,
    WifiDeleteConfirm,
};

QueueHandle_t s_key_queue;
std::atomic<bool> s_config_mode{false};
std::atomic<bool> s_connected{false};
std::atomic<bool> s_settings_mode{false};
std::atomic<bool> s_sntp_started{false};
std::atomic<bool> s_config_can_cancel{false};
TaskHandle_t s_catalog_task;
TaskHandle_t s_wifi_scan_task;
TaskHandle_t s_wifi_connect_task;
std::mutex s_wifi_ui_mutex;
ConfigInputMode s_config_input_mode = ConfigInputMode::NetworkList;
std::array<WifiNetworkView, kMaxVisibleNetworks> s_wifi_networks = {};
std::size_t s_wifi_network_count;
std::size_t s_wifi_network_selected;
std::string s_wifi_selected_ssid;
std::string s_wifi_password;
WifiKeyboardPage s_wifi_keyboard_page = WifiKeyboardPage::Lower;
std::size_t s_wifi_key_selected;
std::string s_config_ap_ssid;
SettingsInputMode s_settings_input_mode = SettingsInputMode::Menu;
std::size_t s_settings_selected;
std::size_t s_saved_wifi_index;
std::mutex s_catalog_mutex;
uint32_t s_catalog_generation;
std::size_t s_source_index = 0;  // current kPodcastSources[] entry

void switch_source(int delta);          // defined after start_catalog_refresh()
void start_catalog_refresh();           // defined below, called by switch_source()

int button_from_mv(int millivolts) {
    if (millivolts < 0 || millivolts >= 1900) return -1;
    if (millivolts < 150) return BSP_BTN_UP;
    if (millivolts < 447) return BSP_BTN_DOWN;
    return BSP_BTN_OK;
}

void button_task(void *) {
    int candidate = -1;
    int stable = -1;
    uint8_t samples = 0;
    TickType_t pressed_at = 0;
    bool long_sent = false;

    while (true) {
        const int current = button_from_mv(bsp_button_read_mv());
        if (current == candidate) {
            if (samples < 3) ++samples;
        } else {
            candidate = current;
            samples = 1;
        }

        if (samples >= 3 && candidate != stable) {
            const int previous = stable;
            stable = candidate;
            if (stable >= 0) {
                pressed_at = xTaskGetTickCount();
                long_sent = false;
            } else if (previous >= 0 && !long_sent) {
                const KeyMessage message = {static_cast<bsp_btn_t>(previous), BSP_BTN_CLICK};
                xQueueSend(s_key_queue, &message, 0);
            }
        }

        if (stable >= 0 && !long_sent &&
            xTaskGetTickCount() - pressed_at >= pdMS_TO_TICKS(850)) {
            const KeyMessage message = {static_cast<bsp_btn_t>(stable), BSP_BTN_LONG};
            xQueueSend(s_key_queue, &message, 0);
            long_sent = true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void enter_wifi_config(bool clear_saved_networks, bool allow_cancel = false) {
    if (s_config_mode.load()) return;
    ESP_LOGI(kTag, "Entering Wi-Fi configuration mode");
    podcast_player_set_network(false);
    if (clear_saved_networks) SsidManager::GetInstance().Clear();
    s_config_can_cancel.store(allow_cancel &&
                              !SsidManager::GetInstance().GetSsidList().empty());
    WifiManager::GetInstance().StartConfigAp();
}

void show_saved_wifi() {
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    std::array<const char *, 10> names = {};
    const std::size_t count = std::min(saved.size(), names.size());
    for (std::size_t i = 0; i < count; ++i) names[i] = saved[i].ssid.c_str();
    s_settings_selected = std::min(s_settings_selected, count);
    podcast_ui_show_saved_wifi(names.data(), count, s_settings_selected);
}

std::string saved_wifi_ssid(std::size_t index) {
    const auto &saved = SsidManager::GetInstance().GetSsidList();
    return index < saved.size() ? saved[index].ssid : std::string();
}

void show_wifi_action() {
    const std::string ssid = saved_wifi_ssid(s_saved_wifi_index);
    if (ssid.empty()) {
        s_settings_input_mode = SettingsInputMode::SavedWifi;
        s_settings_selected = 0;
        show_saved_wifi();
        return;
    }
    podcast_ui_show_wifi_action(ssid.c_str(), s_settings_selected);
}

void show_wifi_delete_confirm() {
    const std::string ssid = saved_wifi_ssid(s_saved_wifi_index);
    if (ssid.empty()) {
        s_settings_input_mode = SettingsInputMode::SavedWifi;
        s_settings_selected = 0;
        show_saved_wifi();
        return;
    }
    podcast_ui_show_wifi_delete_confirm(ssid.c_str(), s_settings_selected);
}

void show_wifi_list(bool scanning) {
    std::array<WifiNetworkView, kMaxVisibleNetworks> networks = {};
    std::size_t count = 0;
    std::size_t selected = 0;
    std::string setup_ssid;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        networks = s_wifi_networks;
        count = s_wifi_network_count;
        selected = count ? std::min(s_wifi_network_selected, count - 1) : 0;
        setup_ssid = s_config_ap_ssid;
    }
    podcast_ui_show_wifi_list(networks.data(), count, selected, scanning,
                              setup_ssid.c_str(), s_config_can_cancel.load());
}

void show_wifi_keyboard(const char *status = nullptr) {
    std::string ssid;
    std::size_t password_length = 0;
    WifiKeyboardPage page;
    std::size_t selected = 0;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        ssid = s_wifi_selected_ssid;
        password_length = s_wifi_password.size();
        page = s_wifi_keyboard_page;
        selected = s_wifi_key_selected;
    }
    podcast_ui_show_wifi_keyboard(ssid.c_str(), password_length, page, selected,
                                  status);
}

void wifi_scan_task(void *) {
    for (int attempt = 0; attempt < 16 && s_config_mode.load(); ++attempt) {
        auto access_points = WifiManager::GetInstance().GetConfigAccessPoints();
        if (!access_points.empty()) {
            std::sort(access_points.begin(), access_points.end(),
                      [](const wifi_ap_record_t &left, const wifi_ap_record_t &right) {
                          return left.rssi > right.rssi;
                      });
            std::array<WifiNetworkView, kMaxVisibleNetworks> networks = {};
            std::size_t count = 0;
            for (const auto &access_point : access_points) {
                const char *ssid = reinterpret_cast<const char *>(access_point.ssid);
                if (!ssid[0]) continue;
                bool duplicate = false;
                for (std::size_t i = 0; i < count; ++i) {
                    if (std::strncmp(networks[i].ssid, ssid,
                                     sizeof(networks[i].ssid)) == 0) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate || count >= networks.size()) continue;
                std::snprintf(networks[count].ssid, sizeof(networks[count].ssid),
                              "%.*s", 32, ssid);
                networks[count].rssi = access_point.rssi;
                networks[count].secure = access_point.authmode != WIFI_AUTH_OPEN;
                ++count;
            }
            if (count) {
                {
                    std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                    s_wifi_networks = networks;
                    s_wifi_network_count = count;
                    s_wifi_network_selected = 0;
                }
                show_wifi_list(false);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }
    bool show_empty_list = false;
    if (s_config_mode.load()) {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        show_empty_list = s_wifi_network_count == 0;
    }
    if (show_empty_list && s_config_mode.load()) show_wifi_list(false);
    s_wifi_scan_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_scan_ui() {
    if (s_wifi_scan_task) return;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_wifi_network_count = 0;
        s_wifi_network_selected = 0;
    }
    show_wifi_list(true);
    if (xTaskCreate(wifi_scan_task, "wifi_screen_scan", 4096, nullptr, 4,
                    &s_wifi_scan_task) != pdPASS) {
        s_wifi_scan_task = nullptr;
        show_wifi_list(false);
    }
}

void wifi_connect_task(void *) {
    std::string ssid;
    std::string password;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        ssid = s_wifi_selected_ssid;
        password = s_wifi_password;
    }
    const bool connected = WifiManager::GetInstance().ConfigureNetwork(ssid, password);
    if (!connected && s_config_mode.load()) {
        {
            std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
            s_config_input_mode = ConfigInputMode::Keyboard;
        }
        show_wifi_keyboard("连接失败");
    }
    s_wifi_connect_task = nullptr;
    vTaskDelete(nullptr);
}

void start_wifi_connection() {
    if (s_wifi_connect_task) return;
    std::string ssid;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        if (s_wifi_selected_ssid.empty()) return;
        s_config_input_mode = ConfigInputMode::Connecting;
        ssid = s_wifi_selected_ssid;
    }
    podcast_ui_show_wifi_connecting(ssid.c_str(), "正在连接");
    if (xTaskCreate(wifi_connect_task, "wifi_screen_join", 5120, nullptr, 5,
                    &s_wifi_connect_task) != pdPASS) {
        s_wifi_connect_task = nullptr;
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        s_config_input_mode = ConfigInputMode::Keyboard;
    }
}

void handle_config_input(const KeyMessage &message) {
    ConfigInputMode mode;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        mode = s_config_input_mode;
    }
    if (mode == ConfigInputMode::Connecting) return;

    if (mode == ConfigInputMode::NetworkList) {
        if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
            if (s_config_can_cancel.load(std::memory_order_acquire)) {
                WifiManager::GetInstance().StopConfigAp();
            } else {
                start_wifi_scan_ui();
            }
            return;
        }
        bool redraw = false;
        bool open_keyboard = false;
        {
            std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
            if (s_wifi_network_count == 0) return;
            if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
                s_wifi_network_selected =
                    (s_wifi_network_selected + s_wifi_network_count - 1) %
                    s_wifi_network_count;
                redraw = true;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
                s_wifi_network_selected =
                    (s_wifi_network_selected + 1) % s_wifi_network_count;
                redraw = true;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
                s_wifi_selected_ssid = s_wifi_networks[s_wifi_network_selected].ssid;
                s_wifi_password.clear();
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
                s_config_input_mode = ConfigInputMode::Keyboard;
                open_keyboard = true;
            }
        }
        if (redraw) show_wifi_list(false);
        if (open_keyboard) show_wifi_keyboard();
        return;
    }

    if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
        start_wifi_connection();
        return;
    }

    bool redraw = false;
    bool connect = false;
    bool back_to_list = false;
    {
        std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
        const std::size_t count = podcast_ui_wifi_key_count(s_wifi_keyboard_page);
        if (!count) return;
        if (message.button == BSP_BTN_UP && message.event == BSP_BTN_CLICK) {
            s_wifi_key_selected = (s_wifi_key_selected + count - 1) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_DOWN && message.event == BSP_BTN_CLICK) {
            s_wifi_key_selected = (s_wifi_key_selected + 1) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_UP && message.event == BSP_BTN_LONG) {
            s_wifi_key_selected = (s_wifi_key_selected + count - std::min<std::size_t>(6, count)) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_DOWN && message.event == BSP_BTN_LONG) {
            s_wifi_key_selected = (s_wifi_key_selected + std::min<std::size_t>(6, count)) % count;
            redraw = true;
        } else if (message.button == BSP_BTN_OK && message.event == BSP_BTN_CLICK) {
            const char *key = podcast_ui_wifi_key(s_wifi_keyboard_page, s_wifi_key_selected);
            if (std::strcmp(key, "ABC") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Upper;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "abc") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "123") == 0) {
                s_wifi_keyboard_page = WifiKeyboardPage::Symbols;
                s_wifi_key_selected = 0;
            } else if (std::strcmp(key, "<") == 0) {
                if (!s_wifi_password.empty()) s_wifi_password.pop_back();
                else {
                    s_config_input_mode = ConfigInputMode::NetworkList;
                    back_to_list = true;
                }
            } else if (std::strcmp(key, "GO") == 0) {
                connect = true;
            } else if (s_wifi_password.size() < 63) {
                s_wifi_password += key;
            }
            redraw = true;
        }
    }
    if (back_to_list) show_wifi_list(false);
    else if (connect) start_wifi_connection();
    else if (redraw) show_wifi_keyboard();
}

void redraw_settings_page() {
    switch (s_settings_input_mode) {
        case SettingsInputMode::Menu:
            podcast_ui_show_settings(PodcastSettingsPage::Menu, s_settings_selected,
                                     podcast_player_volume());
            break;
        case SettingsInputMode::Volume:
            podcast_ui_show_settings(PodcastSettingsPage::Volume, 0,
                                     podcast_player_volume());
            break;
        case SettingsInputMode::SleepTimer:
            podcast_ui_show_settings(PodcastSettingsPage::SleepTimer,
                                     s_settings_selected, podcast_player_volume());
            break;
        case SettingsInputMode::Display:
            podcast_ui_show_settings(PodcastSettingsPage::Display, s_settings_selected,
                                     podcast_player_volume());
            break;
        case SettingsInputMode::WifiMenu:
            podcast_ui_show_settings(PodcastSettingsPage::WifiMenu, s_settings_selected,
                                     podcast_player_volume());
            break;
        case SettingsInputMode::SavedWifi:
            show_saved_wifi();
            break;
        case SettingsInputMode::WifiAction:
            show_wifi_action();
            break;
        case SettingsInputMode::WifiDeleteConfirm:
            show_wifi_delete_confirm();
            break;
    }
}

void input_task(void *) {
    KeyMessage message;
    while (true) {
        if (xQueueReceive(s_key_queue, &message, portMAX_DELAY) != pdTRUE) continue;
        if (!radio_display_note_activity()) continue;

        if (s_config_mode.load(std::memory_order_acquire)) {
            handle_config_input(message);
            continue;
        }

        if (s_settings_mode.load(std::memory_order_acquire)) {
            // Long OK always backs up one settings level.
            if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
                switch (s_settings_input_mode) {
                    case SettingsInputMode::Menu:
                        s_settings_mode.store(false);
                        podcast_ui_show_main();
                        break;
                    case SettingsInputMode::Volume:
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 0;
                        redraw_settings_page();
                        break;
                    case SettingsInputMode::SleepTimer:
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 1;
                        redraw_settings_page();
                        break;
                    case SettingsInputMode::Display:
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 2;
                        redraw_settings_page();
                        break;
                    case SettingsInputMode::WifiMenu:
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 3;
                        redraw_settings_page();
                        break;
                    case SettingsInputMode::SavedWifi:
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 1;
                        redraw_settings_page();
                        break;
                    case SettingsInputMode::WifiAction:
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = s_saved_wifi_index;
                        redraw_settings_page();
                        break;
                    case SettingsInputMode::WifiDeleteConfirm:
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 1;
                        redraw_settings_page();
                        break;
                }
                continue;
            }

            if (s_settings_input_mode == SettingsInputMode::Volume) {
                if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
                    podcast_player_set_volume(podcast_player_volume() + 5);
                } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
                    const uint8_t volume = podcast_player_volume();
                    podcast_player_set_volume(volume > 15 ? volume - 5 : 10);
                } else if (message.event == BSP_BTN_CLICK &&
                           message.button == BSP_BTN_OK) {
                    s_settings_input_mode = SettingsInputMode::Menu;
                    s_settings_selected = 0;
                    podcast_ui_show_settings(PodcastSettingsPage::Menu, 0,
                                             podcast_player_volume());
                }
                continue;
            }

            std::size_t count = 5;  // main settings menu rows
            if (s_settings_input_mode == SettingsInputMode::SleepTimer) {
                count = 6;
            } else if (s_settings_input_mode == SettingsInputMode::Display ||
                       s_settings_input_mode == SettingsInputMode::WifiMenu ||
                       s_settings_input_mode == SettingsInputMode::WifiAction) {
                count = 3;
            } else if (s_settings_input_mode == SettingsInputMode::WifiDeleteConfirm) {
                count = 2;
            } else if (s_settings_input_mode == SettingsInputMode::SavedWifi) {
                count = SsidManager::GetInstance().GetSsidList().size() + 1;
            }
            if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
                s_settings_selected = (s_settings_selected + count - 1) % count;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
                s_settings_selected = (s_settings_selected + 1) % count;
            } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
                if (s_settings_input_mode == SettingsInputMode::Menu) {
                    if (s_settings_selected == 0) {
                        s_settings_input_mode = SettingsInputMode::Volume;
                        podcast_ui_show_settings(PodcastSettingsPage::Volume, 0,
                                                 podcast_player_volume());
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::SleepTimer;
                        s_settings_selected = radio_sleep_timer_selection();
                        redraw_settings_page();
                    } else if (s_settings_selected == 2) {
                        s_settings_input_mode = SettingsInputMode::Display;
                        s_settings_selected = 0;
                        redraw_settings_page();
                    } else if (s_settings_selected == 3) {
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 0;
                        redraw_settings_page();
                    } else {
                        // row 4: back
                        s_settings_mode.store(false);
                        podcast_ui_show_main();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::SleepTimer) {
                    static constexpr uint16_t kSleepMinutes[] = {0, 15, 30, 60, 90};
                    if (s_settings_selected < 5) {
                        radio_sleep_timer_set_minutes(kSleepMinutes[s_settings_selected]);
                    }
                    s_settings_input_mode = SettingsInputMode::Menu;
                    s_settings_selected = 1;
                    podcast_ui_show_settings(PodcastSettingsPage::Menu, 1,
                                             podcast_player_volume());
                } else if (s_settings_input_mode == SettingsInputMode::Display) {
                    if (s_settings_selected == 0) {
                        radio_display_set_brightness_selection(
                            radio_display_brightness_selection() + 1);
                    } else if (s_settings_selected == 1) {
                        radio_display_set_timeout_selection(
                            radio_display_timeout_selection() + 1);
                    } else {
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 2;
                    }
                    redraw_settings_page();
                } else if (s_settings_input_mode == SettingsInputMode::WifiMenu) {
                    if (s_settings_selected == 0) {
                        s_settings_mode.store(false);
                        enter_wifi_config(false, true);
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = 0;
                        redraw_settings_page();
                    } else {
                        s_settings_input_mode = SettingsInputMode::Menu;
                        s_settings_selected = 3;
                        redraw_settings_page();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::SavedWifi) {
                    const std::size_t saved_count =
                        SsidManager::GetInstance().GetSsidList().size();
                    if (s_settings_selected < saved_count) {
                        s_saved_wifi_index = s_settings_selected;
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 0;
                        redraw_settings_page();
                    } else {
                        s_settings_input_mode = SettingsInputMode::WifiMenu;
                        s_settings_selected = 1;
                        redraw_settings_page();
                    }
                } else if (s_settings_input_mode == SettingsInputMode::WifiAction) {
                    if (s_settings_selected == 0) {
                        SsidManager::GetInstance().SetDefaultSsid(
                            static_cast<int>(s_saved_wifi_index));
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = 0;
                        redraw_settings_page();
                    } else if (s_settings_selected == 1) {
                        s_settings_input_mode = SettingsInputMode::WifiDeleteConfirm;
                        s_settings_selected = 0;
                        redraw_settings_page();
                    } else {
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected = s_saved_wifi_index;
                        redraw_settings_page();
                    }
                } else if (s_settings_input_mode ==
                           SettingsInputMode::WifiDeleteConfirm) {
                    if (s_settings_selected == 1) {
                        SsidManager::GetInstance().RemoveSsid(
                            static_cast<int>(s_saved_wifi_index));
                        const std::size_t saved_count =
                            SsidManager::GetInstance().GetSsidList().size();
                        s_settings_input_mode = SettingsInputMode::SavedWifi;
                        s_settings_selected =
                            std::min(s_saved_wifi_index, saved_count);
                    } else {
                        s_settings_input_mode = SettingsInputMode::WifiAction;
                        s_settings_selected = 1;
                    }
                    redraw_settings_page();
                }
                continue;
            }
            continue;
        }

        if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_UP) {
            podcast_player_select_relative(-1);
        } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_DOWN) {
            podcast_player_select_relative(1);
        } else if (message.event == BSP_BTN_CLICK && message.button == BSP_BTN_OK) {
            podcast_player_toggle();
        } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_UP) {
            switch_source(-1);
        } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_DOWN) {
            switch_source(1);
        } else if (message.event == BSP_BTN_LONG && message.button == BSP_BTN_OK) {
            s_settings_mode.store(true);
            s_settings_input_mode = SettingsInputMode::Menu;
            s_settings_selected = 0;
            podcast_ui_show_settings(PodcastSettingsPage::Menu, 0,
                                     podcast_player_volume());
        }
    }
}

void start_sntp_once() {
    bool expected = false;
    if (!s_sntp_started.compare_exchange_strong(expected, true)) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.start = true;
    config.server_from_dhcp = false;
    config.renew_servers_after_new_IP = false;
    const esp_err_t error = esp_netif_sntp_init(&config);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(error));
        s_sntp_started.store(false);
    }
}

// Compact source indicator shown above the episode list, e.g. "2/5 硅谷101"
// or "2/5 硅谷101 · 更新中". s_podcast_name_text is 48 bytes; keep labels short.
std::string source_display_label(std::size_t index, const char *suffix) {
    std::string label = std::to_string(index + 1) + "/" +
                        std::to_string(kPodcastSourceCount) + " " +
                        kPodcastSources[index].name;
    if (suffix) {
        label += " ";
        label += suffix;
    }
    return label;
}

void switch_source(int delta) {
    if (s_config_mode.load() || s_settings_mode.load()) return;
    std::size_t index = 0;
    {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        index = (s_source_index + kPodcastSourceCount + delta) %
                kPodcastSourceCount;
        s_source_index = index;
        ++s_catalog_generation;  // invalidate any in-flight fetch for the old source
    }
    podcast_player_set_playing(false);
    const PodcastEpisode empty{};
    podcast_ui_set_episode(0, 0, empty);
    podcast_ui_set_podcast_name(source_display_label(index, nullptr).c_str());
    start_catalog_refresh();
}

void start_catalog_refresh() {
    bool task_running = false;
    std::size_t src_index = 0;
    {
        std::lock_guard<std::mutex> lock(s_catalog_mutex);
        ++s_catalog_generation;
        task_running = s_catalog_task != nullptr;
        src_index = s_source_index;
    }
    if (!task_running &&
        xTaskCreate([](void *) {
            // Loop so a source switch mid-fetch re-runs for the current source
            // instead of dropping the refresh.
            while (true) {
                const PodcastSource *src = nullptr;
                uint32_t generation = 0;
                {
                    std::lock_guard<std::mutex> lock(s_catalog_mutex);
                    generation = s_catalog_generation;
                    src = &kPodcastSources[s_source_index];
                }
                auto *episodes = static_cast<PodcastEpisode *>(
                    std::calloc(kEpisodeCapacity, sizeof(PodcastEpisode)));
                if (!episodes) {
                    s_catalog_task = nullptr;
                    vTaskDelete(nullptr);
                }
                const std::size_t count = podcast_catalog_fetch(
                    src->feed_url, episodes, kEpisodeCapacity);
                bool restart = false;
                {
                    std::lock_guard<std::mutex> lock(s_catalog_mutex);
                    if (generation != s_catalog_generation) {
                        restart = true;  // user switched sources; retry current one
                    } else if (s_connected.load(std::memory_order_acquire)) {
                        if (count > 0) {
                            podcast_player_replace_episodes(episodes, count);
                        }
                        podcast_ui_set_podcast_name(
                            source_display_label(s_source_index, nullptr).c_str());
                        podcast_player_set_network(true);
                    }
                }
                std::free(episodes);
                if (!restart) break;
            }
            s_catalog_task = nullptr;
            vTaskDelete(nullptr);
        }, "podcast_catalog", 8192, nullptr, 5, &s_catalog_task) != pdPASS) {
        s_catalog_task = nullptr;
    }
    podcast_player_set_network(false);
    podcast_ui_set_podcast_name(
        source_display_label(src_index, "更新中").c_str());
    podcast_ui_set_playback(PodcastPlaybackState::Buffering, "正在更新节目单");
    if (!task_running && !s_catalog_task) {
        podcast_player_set_network(true);
    }
}

void wifi_event(WifiEvent event, const std::string &data) {
    auto &manager = WifiManager::GetInstance();
    switch (event) {
        case WifiEvent::Scanning:
            podcast_ui_set_network(false, "正在寻找网络");
            break;
        case WifiEvent::Connecting:
            podcast_ui_set_network(false, data.c_str());
            podcast_ui_set_playback(PodcastPlaybackState::Stopped, "正在连接无线网络");
            break;
        case WifiEvent::Connected:
            ESP_LOGI(kTag, "Wi-Fi connected: %s, IP=%s", data.c_str(),
                     manager.GetIpAddress().c_str());
            s_connected.store(true);
            s_config_mode.store(false);
            s_config_can_cancel.store(false);
            s_settings_mode.store(false);
            podcast_ui_show_main();
            podcast_ui_set_network(true, data.c_str());
            podcast_ui_set_episode(podcast_player_index(), podcast_player_count(),
                                   podcast_player_episode(podcast_player_index()));
            manager.SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
            start_sntp_once();
            start_catalog_refresh();
            break;
        case WifiEvent::Disconnected:
            s_connected.store(false);
            podcast_player_set_network(false);
            podcast_ui_set_network(false, "网络已断开");
            break;
        case WifiEvent::ConfigModeEnter: {
            s_config_mode.store(true);
            s_settings_mode.store(false);
            s_connected.store(false);
            podcast_player_set_network(false);
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_config_input_mode = ConfigInputMode::NetworkList;
                s_wifi_networks = {};
                s_wifi_network_count = 0;
                s_wifi_network_selected = 0;
                s_wifi_selected_ssid.clear();
                s_wifi_password.clear();
                s_wifi_keyboard_page = WifiKeyboardPage::Lower;
                s_wifi_key_selected = 0;
                s_config_ap_ssid = manager.GetApSsid();
            }
            start_wifi_scan_ui();
            break;
        }
        case WifiEvent::ConfigModeExit: {
            s_config_mode.store(false);
            s_config_can_cancel.store(false);
            {
                std::lock_guard<std::mutex> lock(s_wifi_ui_mutex);
                s_config_input_mode = ConfigInputMode::NetworkList;
                s_wifi_networks = {};
                s_wifi_network_count = 0;
                s_wifi_network_selected = 0;
                s_wifi_selected_ssid.clear();
                s_wifi_password.clear();
                s_wifi_key_selected = 0;
                s_config_ap_ssid.clear();
            }
            podcast_ui_show_main();
            podcast_ui_set_network(false, "正在连接网络");
            if (!manager.IsConnected()) manager.StartStation();
            break;
        }
    }
}

void connection_watchdog_task(void *) {
    while (true) {
        int waiting_seconds = 0;
        while (!s_connected.load() && !s_config_mode.load() && waiting_seconds < 30) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ++waiting_seconds;
        }
        if (!s_connected.load() && !s_config_mode.load()) {
            ESP_LOGW(kTag, "No usable network after 30 seconds; opening setup portal");
            enter_wifi_config(false);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

extern "C" void app_main(void) {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    ESP_ERROR_CHECK(bsp_display_init());
    if (!bsp_lvgl_init()) {
        ESP_LOGE(kTag, "Display initialization failed");
        return;
    }
    bsp_display_backlight(72);
    if (!radio_display_settings_init()) {
        ESP_LOGW(kTag, "Display settings initialization failed");
    }
    bsp_battery_init();
    if (!podcast_ui_init()) {
        ESP_LOGE(kTag, "UI initialization failed");
        return;
    }
    if (!podcast_player_init()) {
        podcast_ui_set_playback(PodcastPlaybackState::Error, "音频初始化失败");
        ESP_LOGE(kTag, "Audio player initialization failed");
        return;
    }
    if (!radio_sleep_timer_init()) {
        ESP_LOGW(kTag, "Sleep timer initialization failed");
    }

    s_key_queue = xQueueCreate(10, sizeof(KeyMessage));
    ESP_ERROR_CHECK(bsp_button_init(nullptr, nullptr));
    if (!s_key_queue ||
        xTaskCreate(button_task, "podcast_keys", 2048, nullptr, 6, nullptr) != pdPASS ||
        xTaskCreate(input_task, "podcast_input", 4096, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Button initialization failed");
        return;
    }

    auto &manager = WifiManager::GetInstance();
    WifiManagerConfig config;
    config.ssid_prefix = "Podcast";
    config.fixed_ap_ssid = "Podcast";
    config.language = "zh-CN";
    config.station_hostname = "ai-podcast";
    config.station_scan_min_interval_seconds = 5;
    config.station_scan_max_interval_seconds = 30;
    if (!manager.Initialize(config)) {
        podcast_ui_set_playback(PodcastPlaybackState::Error, "无线网络初始化失败");
        return;
    }
    manager.SetEventCallback(wifi_event);

    const auto &saved_networks = SsidManager::GetInstance().GetSsidList();
    if (saved_networks.empty()) {
        manager.StartConfigAp();
    } else {
        manager.StartStation();
    }

    if (xTaskCreate(connection_watchdog_task, "podcast_wifi_guard", 3072, nullptr, 4,
                    nullptr) != pdPASS) {
        ESP_LOGE(kTag, "Wi-Fi watchdog task failed");
    }
    ESP_LOGI(kTag, "Podcast player ready");
}
