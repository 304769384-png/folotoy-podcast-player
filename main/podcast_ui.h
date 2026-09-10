#pragma once

#include "podcast_player.h"

#include <cstddef>
#include <cstdint>

struct WifiNetworkView {
    char ssid[33];
    int8_t rssi;
    bool secure;
};

enum class WifiKeyboardPage {
    Lower,
    Upper,
    Symbols,
};

enum class PodcastSettingsPage {
    Menu,
    Volume,
    SleepTimer,
    Display,
    WifiMenu,
    SavedWifi,
    WifiAction,
    WifiDeleteConfirm,
};

bool podcast_ui_init();
void podcast_ui_show_main();
void podcast_ui_show_config(const char *ssid, const char *url);
void podcast_ui_show_wifi_list(const WifiNetworkView *networks, std::size_t count,
                               std::size_t selected, bool scanning,
                               const char *setup_ap_ssid, bool can_cancel = false);
void podcast_ui_show_wifi_keyboard(const char *ssid, std::size_t password_length,
                                   WifiKeyboardPage page, std::size_t selected,
                                   const char *status = nullptr);
void podcast_ui_show_wifi_connecting(const char *ssid, const char *detail);
const char *podcast_ui_wifi_key(WifiKeyboardPage page, std::size_t index);
std::size_t podcast_ui_wifi_key_count(WifiKeyboardPage page);
void podcast_ui_set_network(bool connected, const char *detail);
void podcast_ui_set_podcast_name(const char *name);
void podcast_ui_set_episode(std::size_t index, std::size_t count,
                            const PodcastEpisode &episode);
void podcast_ui_set_playback(PodcastPlaybackState state, const char *detail = nullptr);
void podcast_ui_set_audio_levels(const uint8_t *levels, std::size_t count);
void podcast_ui_show_settings(PodcastSettingsPage page, std::size_t selected,
                              uint8_t volume);
void podcast_ui_show_saved_wifi(const char *const *ssids, std::size_t count,
                                std::size_t selected);
void podcast_ui_show_wifi_action(const char *ssid, std::size_t selected);
void podcast_ui_show_wifi_delete_confirm(const char *ssid, std::size_t selected);
void podcast_ui_set_volume(uint8_t volume);
