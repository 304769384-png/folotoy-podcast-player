#include "podcast_ui.h"

#include "pod_font.h"
#include "radio_display_settings.h"
#include "radio_sleep_timer.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr uint32_t kBackground = 0x071017;
constexpr uint32_t kPanel = 0x0D1B23;
constexpr uint32_t kPanelSoft = 0x11262E;
constexpr uint32_t kGrid = 0x24404A;
constexpr uint32_t kText = 0xF4F0DE;
constexpr uint32_t kMuted = 0x849BA0;
constexpr uint32_t kAmber = 0xFFB74D;
constexpr uint32_t kAmberSoft = 0x7F5A29;
constexpr uint32_t kGreen = 0x4ED39A;
constexpr uint32_t kRed = 0xFF5D62;

constexpr std::size_t kMeterCount = 18;
constexpr std::size_t kWifiVisibleRows = 6;
constexpr std::size_t kWifiMaxKeys = 34;

enum class Page {
    Main,
    Settings,
    Config,
    WifiList,
    WifiKeyboard,
    WifiConnecting,
};

constexpr const char *kLowerKeys[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
    "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
    "y", "z", "ABC", "123", "<", "GO",
};
constexpr const char *kUpperKeys[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L",
    "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X",
    "Y", "Z", "abc", "123", "<", "GO",
};
constexpr const char *kSymbolKeys[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "-", "_",
    ".", "@", "#", "$", "%", "&", "*", "!", "+", "=", "?", "/",
    ":", ";", "(", ")", "[", "]", "abc", "ABC", "<", "GO",
};

lv_obj_t *s_screen;
lv_obj_t *s_main_group;
lv_obj_t *s_episode_number;
lv_obj_t *s_episode_title;
lv_obj_t *s_podcast_name;
lv_obj_t *s_play_icon;
lv_obj_t *s_status;
lv_obj_t *s_clock;
lv_obj_t *s_battery;
lv_obj_t *s_sleep_timer;
lv_obj_t *s_wifi_bars[3];
lv_obj_t *s_meter[kMeterCount];
lv_obj_t *s_volume_value;
lv_obj_t *s_volume_bar;
lv_obj_t *s_settings_rows[8];
lv_obj_t *s_settings_labels[8];
lv_obj_t *s_pair_dot;
lv_obj_t *s_wifi_state;
lv_obj_t *s_wifi_list_hint;
lv_obj_t *s_wifi_row_box[kWifiVisibleRows];
lv_obj_t *s_wifi_row_ssid[kWifiVisibleRows];
lv_obj_t *s_wifi_row_info[kWifiVisibleRows];
lv_obj_t *s_wifi_keyboard_network;
lv_obj_t *s_wifi_password_mask;
lv_obj_t *s_wifi_keyboard_status;
lv_obj_t *s_wifi_key_box[kWifiMaxKeys];
lv_obj_t *s_wifi_key_label[kWifiMaxKeys];

Page s_page = Page::Main;
bool s_network_connected;
uint8_t s_volume = 55;
PodcastSettingsPage s_settings_page = PodcastSettingsPage::Menu;
std::size_t s_settings_selected;
std::size_t s_settings_row_count;
std::size_t s_episode_index;
std::size_t s_episode_count = 1;
char s_episode_title_text[sizeof(PodcastEpisode::title)] = "正在加载节目";
char s_podcast_name_text[48] = "PODCAST";
char s_network_detail[64] = "等待网络";
char s_playback_detail[64] = "准备播放";
PodcastPlaybackState s_playback_state = PodcastPlaybackState::Stopped;
std::atomic<uint32_t> s_level_generation{0};
uint8_t s_levels[kMeterCount] = {};
uint32_t s_last_level_generation;
uint8_t s_animation;
uint32_t s_battery_ticks;
uint32_t s_last_sleep_seconds = UINT32_MAX;


lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int width, int height,
                   uint32_t color, int radius = 0) {
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, radius, 0);
    return object;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int width,
                     uint32_t color, const lv_font_t *font = &pod_font) {
    lv_obj_t *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_text_opa(object, LV_OPA_COVER, 0);
    lv_label_set_long_mode(object, LV_LABEL_LONG_DOT);
    return object;
}

void set_centered(lv_obj_t *object) {
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
}

const char *playback_text() {
    switch (s_playback_state) {
        case PodcastPlaybackState::Stopped: return "已暂停";
        case PodcastPlaybackState::Connecting: return "正在连接节目";
        case PodcastPlaybackState::Buffering: return "正在缓冲";
        case PodcastPlaybackState::Playing: return "正在播放";
        case PodcastPlaybackState::Reconnecting: return "信号中断，正在重连";
        case PodcastPlaybackState::Error: return "暂时无法播放";
    }
    return "准备播放";
}

void apply_network() {
    for (int i = 0; i < 3; ++i) {
        if (s_wifi_bars[i]) {
            lv_obj_set_style_bg_color(s_wifi_bars[i],
                                      lv_color_hex(s_network_connected ? kGreen : kMuted), 0);
            lv_obj_set_style_opa(s_wifi_bars[i],
                                 s_network_connected ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }
}

void apply_episode() {
    if (!s_episode_title || !s_episode_number || !s_podcast_name) return;
    char preset[20];
    std::snprintf(preset, sizeof(preset), "EP %02u / %02u",
                  static_cast<unsigned>(s_episode_index + 1),
                  static_cast<unsigned>(s_episode_count));
    lv_label_set_text(s_episode_number, preset);
    lv_label_set_text(s_episode_title, s_episode_title_text);
    lv_label_set_text(s_podcast_name, s_podcast_name_text);
}

void apply_playback() {
    if (!s_status || !s_play_icon) return;
    lv_label_set_text(s_status, s_playback_detail[0] ? s_playback_detail : playback_text());
    const bool playing = s_playback_state == PodcastPlaybackState::Playing ||
                         s_playback_state == PodcastPlaybackState::Buffering ||
                         s_playback_state == PodcastPlaybackState::Connecting ||
                         s_playback_state == PodcastPlaybackState::Reconnecting;
    lv_label_set_text(s_play_icon, playing ? "II" : ">" );
    lv_obj_set_style_text_color(s_play_icon,
                                lv_color_hex(s_playback_state == PodcastPlaybackState::Error
                                                 ? kRed
                                                 : kAmber), 0);
}

void apply_volume() {
    if (!s_volume_value || !s_volume_bar) return;
    char text[16];
    std::snprintf(text, sizeof(text), "%u%%", s_volume);
    lv_label_set_text(s_volume_value, text);
    lv_bar_set_value(s_volume_bar, s_volume, LV_ANIM_ON);
}

void apply_sleep_timer() {
    if (!s_sleep_timer) return;
    const uint32_t seconds = radio_sleep_timer_remaining_seconds();
    if (seconds == s_last_sleep_seconds) return;
    s_last_sleep_seconds = seconds;
    if (!seconds) {
        lv_label_set_text(s_sleep_timer, "");
        return;
    }
    char text[24];
    std::snprintf(text, sizeof(text), "定时 %02u:%02u",
                  static_cast<unsigned>(seconds / 60U),
                  static_cast<unsigned>(seconds % 60U));
    lv_label_set_text(s_sleep_timer, text);
}

void build_main() {
    lv_obj_clean(s_screen);
    s_page = Page::Main;
    s_episode_number = nullptr;
    s_episode_title = nullptr;
    s_podcast_name = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_volume_value = nullptr;
    s_volume_bar = nullptr;
    s_pair_dot = nullptr;
    s_sleep_timer = nullptr;
    s_clock = nullptr;
    s_battery = nullptr;
    s_last_sleep_seconds = UINT32_MAX;

    s_main_group = make_box(s_screen, 0, 0, 240, 320, kBackground);
    make_label(s_main_group, "PODCAST", 12, 10, 92, kAmber,
               &lv_font_montserrat_14);
    s_clock = make_label(s_main_group, "--:--", 100, 7, 73, kText,
                         &lv_font_montserrat_20);
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_RIGHT, 0);
    s_battery = make_label(s_main_group, "--%", 195, 11, 39, kMuted,
                           &lv_font_montserrat_14);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);

    for (int i = 0; i < 3; ++i) {
        const int height = 3 + i * 3;
        s_wifi_bars[i] = make_box(s_main_group, 178 + i * 5, 20 - height,
                                  3, height, kMuted, 1);
    }
    make_box(s_main_group, 12, 37, 216, 1, kGrid);

    s_episode_number = make_label(s_main_group, "EP 01 / 08", 13, 48, 102, kAmber,
                                  &lv_font_montserrat_14);
    s_sleep_timer = make_label(s_main_group, "", 115, 48, 112, kAmber);
    lv_obj_set_style_text_align(s_sleep_timer, LV_TEXT_ALIGN_RIGHT, 0);

    // Episode card: long titles wrap to multiple lines. pod_font carries the
    // UI glyph set and falls back to the GB2312 CJK font for arbitrary title
    // characters (see lv_font_set_fallback in podcast_ui_init).
    lv_obj_t *card = make_box(s_main_group, 13, 74, 214, 132, kPanel, 8);
    lv_obj_set_style_border_color(card, lv_color_hex(kGrid), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    s_episode_title = make_label(card, s_episode_title_text, 12, 12, 190,
                                 kText, &pod_font);
    lv_label_set_long_mode(s_episode_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_episode_title, LV_TEXT_ALIGN_LEFT, 0);

    s_podcast_name = make_label(s_main_group, s_podcast_name_text, 13, 214,
                                214, kMuted);
    set_centered(s_podcast_name);

    lv_obj_t *meter_panel = make_box(s_main_group, 13, 234, 214, 33, kPanelSoft, 7);
    for (std::size_t i = 0; i < kMeterCount; ++i) {
        s_meter[i] = make_box(meter_panel, 9 + static_cast<int>(i) * 11,
                              23, 6, 3, kAmber, 2);
    }

    s_play_icon = make_label(s_main_group, ">", 13, 274, 24, kAmber,
                             &lv_font_montserrat_20);
    s_status = make_label(s_main_group, "准备播放", 39, 276, 187, kText);
    make_box(s_main_group, 12, 298, 216, 1, kGrid);
    lv_obj_t *hint = make_label(s_main_group, "上下选期  确定播放  长按设置",
                                12, 302, 216, kMuted);
    set_centered(hint);

    apply_network();
    apply_episode();
    apply_playback();
    apply_sleep_timer();
}

void build_config(const char *ssid, const char *url) {
    lv_obj_clean(s_screen);
    s_page = Page::Config;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_clock = nullptr;
    s_battery = nullptr;
    s_sleep_timer = nullptr;
    for (auto &bar : s_wifi_bars) bar = nullptr;
    for (auto &bar : s_meter) bar = nullptr;

    make_label(s_screen, "播客机", 12, 12, 216, kText, &pod_font);
    lv_obj_t *tag = make_label(s_screen, "首次配网", 12, 45, 216, kAmber);
    set_centered(tag);
    s_pair_dot = make_box(s_screen, 112, 77, 16, 16, kRed, LV_RADIUS_CIRCLE);

    lv_obj_t *panel = make_box(s_screen, 18, 111, 204, 132, kPanel, 10);
    lv_obj_set_style_border_color(panel, lv_color_hex(kGrid), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_t *hint = make_label(panel, "手机连接热点", 0, 14, 204, kMuted);
    set_centered(hint);
    lv_obj_t *ssid_label = make_label(panel, ssid && ssid[0] ? ssid : "Podcast",
                                      8, 43, 188, kAmber);
    set_centered(ssid_label);
    hint = make_label(panel, "连接后会自动打开配网页面", 0, 77, 204, kText);
    set_centered(hint);
    lv_obj_t *address = make_label(panel, url && url[0] ? url : "192.168.4.1",
                                   0, 104, 204, kMuted,
                                   &lv_font_montserrat_14);
    set_centered(address);
    hint = make_label(s_screen, "请选择 2.4G 无线网络", 12, 259, 216, kText);
    set_centered(hint);
    hint = make_label(s_screen, "配网完成后自动加载节目", 12, 286, 216, kMuted);
    set_centered(hint);
}

const lv_font_t *ssid_font(const char *ssid) {
    if (!ssid) return &lv_font_montserrat_14;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(ssid); *p; ++p) {
        if (*p >= 0x80) return &pod_font;
    }
    return &lv_font_montserrat_14;
}

void reset_transient_objects(Page page) {
    lv_obj_clean(s_screen);
    s_page = page;
    s_episode_number = nullptr;
    s_episode_title = nullptr;
    s_podcast_name = nullptr;
    s_status = nullptr;
    s_play_icon = nullptr;
    s_volume_value = nullptr;
    s_volume_bar = nullptr;
    s_clock = nullptr;
    s_battery = nullptr;
    s_sleep_timer = nullptr;
    s_pair_dot = nullptr;
    s_wifi_state = nullptr;
    s_wifi_list_hint = nullptr;
    s_wifi_keyboard_network = nullptr;
    s_wifi_password_mask = nullptr;
    s_wifi_keyboard_status = nullptr;
    for (auto &bar : s_wifi_bars) bar = nullptr;
    for (auto &bar : s_meter) bar = nullptr;
    for (auto &row : s_settings_rows) row = nullptr;
    for (auto &label : s_settings_labels) label = nullptr;
    for (auto &box : s_wifi_row_box) box = nullptr;
    for (auto &label : s_wifi_row_ssid) label = nullptr;
    for (auto &label : s_wifi_row_info) label = nullptr;
    for (auto &box : s_wifi_key_box) box = nullptr;
    for (auto &label : s_wifi_key_label) label = nullptr;
}

void apply_settings_selection() {
    for (std::size_t i = 0; i < s_settings_row_count; ++i) {
        if (!s_settings_rows[i] || !s_settings_labels[i]) continue;
        const bool active = i == s_settings_selected;
        lv_obj_set_style_bg_color(s_settings_rows[i],
                                  lv_color_hex(active ? kAmber : kPanel), 0);
        lv_obj_set_style_text_color(s_settings_labels[i],
                                    lv_color_hex(active ? kBackground : kText), 0);
    }
}

void build_settings(PodcastSettingsPage page, std::size_t selected, uint8_t volume) {
    reset_transient_objects(Page::Settings);
    s_settings_page = page;
    s_settings_selected = selected;
    s_volume = volume;
    s_settings_row_count = 0;

    if (page == PodcastSettingsPage::Volume) {
        lv_obj_t *title = make_label(s_screen, "音量", 12, 16, 216, kText,
                                     &pod_font);
        set_centered(title);
        s_volume_value = make_label(s_screen, "55%", 12, 84, 216, kAmber,
                                    &lv_font_montserrat_20);
        set_centered(s_volume_value);
        s_volume_bar = lv_bar_create(s_screen);
        lv_obj_set_pos(s_volume_bar, 30, 136);
        lv_obj_set_size(s_volume_bar, 180, 14);
        lv_bar_set_range(s_volume_bar, 0, 100);
        lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(kGrid), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_volume_bar, lv_color_hex(kAmber), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_volume_bar, 7, LV_PART_MAIN);
        lv_obj_set_style_radius(s_volume_bar, 7, LV_PART_INDICATOR);
        lv_obj_t *hint = make_label(s_screen, "上下调节  确定返回",
                                    12, 276, 216, kMuted);
        set_centered(hint);
        apply_volume();
        return;
    }

    static constexpr const char *kMenuItems[] = {
        "音量", "定时播放", "屏幕设置", "网络设置", "返回",
    };
    static constexpr const char *kSleepTimerItems[] = {
        "无定时", "15 分钟", "30 分钟", "60 分钟", "90 分钟", "返回",
    };
    char brightness_item[32];
    char timeout_item[48];
    std::snprintf(brightness_item, sizeof(brightness_item), "亮度 %u%%",
                  static_cast<unsigned>(radio_display_brightness()));
    const uint16_t timeout = radio_display_timeout_seconds();
    if (!timeout) {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 关闭");
    } else if (timeout < 60) {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 秒",
                      static_cast<unsigned>(timeout));
    } else {
        std::snprintf(timeout_item, sizeof(timeout_item), "自动熄屏 %u 分钟",
                      static_cast<unsigned>(timeout / 60));
    }
    const char *display_items[] = {brightness_item, timeout_item, "返回"};
    static constexpr const char *kWifiMenuItems[] = {
        "连接新网络", "已存网络", "返回",
    };

    const char *const *items = nullptr;
    std::size_t item_count = 0;
    const char *title_text = "设置";
    const char *hint_text = "上下选择  确定进入";
    switch (page) {
        case PodcastSettingsPage::Menu:
            items = kMenuItems;
            item_count = sizeof(kMenuItems) / sizeof(kMenuItems[0]);
            break;
        case PodcastSettingsPage::SleepTimer:
            items = kSleepTimerItems;
            item_count = sizeof(kSleepTimerItems) / sizeof(kSleepTimerItems[0]);
            title_text = "定时播放";
            hint_text = "确定开始  长按返回";
            break;
        case PodcastSettingsPage::Display:
            items = display_items;
            item_count = sizeof(display_items) / sizeof(display_items[0]);
            title_text = "屏幕设置";
            hint_text = "确定切换  长按返回";
            break;
        case PodcastSettingsPage::WifiMenu:
            items = kWifiMenuItems;
            item_count = sizeof(kWifiMenuItems) / sizeof(kWifiMenuItems[0]);
            title_text = "网络设置";
            hint_text = "确定进入  长按返回";
            break;
        default:
            return;
    }

    const bool compact = item_count > 3;
    const bool extra_compact = item_count > 6;
    const bool city_rows = item_count >= 8;
    const int first_y = city_rows ? 34 : (extra_compact ? 39 : (compact ? 45 : 70));
    const int row_height = city_rows ? 27 : (extra_compact ? 29 : (compact ? 32 : 42));
    const int row_gap = city_rows ? 30 : (extra_compact ? 34 : (compact ? 38 : 55));
    s_settings_row_count = item_count;
    if (s_settings_selected >= item_count) s_settings_selected = 0;

    lv_obj_t *title = make_label(s_screen, title_text, 12, 13, 216, kText,
                                 &pod_font);
    set_centered(title);
    for (std::size_t i = 0; i < item_count; ++i) {
        const int y = first_y + static_cast<int>(i) * row_gap;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, row_height, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], items[i], 0,
                                          city_rows ? 5 :
                                          extra_compact ? 5 : (compact ? 7 : 12),
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    lv_obj_t *hint = make_label(s_screen, hint_text, 12, 283, 216, kMuted);
    set_centered(hint);
}

void build_saved_wifi(const char *const *ssids, std::size_t count,
                      std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = PodcastSettingsPage::SavedWifi;
    const std::size_t total = count + 1;
    selected = std::min(selected, total - 1);

    lv_obj_t *title = make_label(s_screen, "已存网络", 12, 12, 216, kText,
                                 &pod_font);
    set_centered(title);
    lv_obj_t *state = make_label(s_screen,
                                 count ? "上下选择  确定设置" : "无保存网络",
                                 12, 40, 216, count ? kMuted : kAmber);
    set_centered(state);

    const std::size_t first = total <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             total - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        if (index >= total) break;
        const bool active = index == selected;
        const int y = 67 + static_cast<int>(row) * 34;
        s_settings_rows[row] = make_box(s_screen, 18, y, 204, 29,
                                        active ? kAmberSoft : kPanel, 5);
        const char *text = index < count ? ssids[index] : "返回";
        s_settings_labels[row] = make_label(
            s_settings_rows[row], text, 8, 6, 188, active ? kText : kMuted,
            index < count ? ssid_font(text) : &pod_font);
        set_centered(s_settings_labels[row]);
    }
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 286, 216, kMuted);
    set_centered(hint);
}

void build_wifi_action(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = PodcastSettingsPage::WifiAction;
    s_settings_selected = selected % 3;
    s_settings_row_count = 3;
    lv_obj_t *title = make_label(s_screen, "网络设置", 12, 13, 216, kText,
                                 &pod_font);
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 45, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    static constexpr const char *kItems[] = {"设为首选", "删除网络", "返回"};
    for (std::size_t i = 0; i < 3; ++i) {
        const int y = 83 + static_cast<int>(i) * 55;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 42, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 12,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
    lv_obj_t *hint = make_label(s_screen, "长按确定返回", 12, 283, 216, kMuted);
    set_centered(hint);
}

void build_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    reset_transient_objects(Page::Settings);
    s_settings_page = PodcastSettingsPage::WifiDeleteConfirm;
    s_settings_selected = selected % 2;
    s_settings_row_count = 2;
    lv_obj_t *title = make_label(s_screen, "确认删除", 12, 18, 216, kText,
                                 &pod_font);
    set_centered(title);
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 55, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    lv_obj_t *warning = make_label(s_screen, "删除后无法自动连接", 12, 88, 216,
                                   kMuted);
    set_centered(warning);
    static constexpr const char *kItems[] = {"返回", "确认删除"};
    for (std::size_t i = 0; i < 2; ++i) {
        const int y = 132 + static_cast<int>(i) * 61;
        s_settings_rows[i] = make_box(s_screen, 24, y, 192, 45, kPanel, 7);
        s_settings_labels[i] = make_label(s_settings_rows[i], kItems[i], 0, 13,
                                          192, kText);
        set_centered(s_settings_labels[i]);
    }
    apply_settings_selection();
}

void update_wifi_list(const WifiNetworkView *networks, std::size_t count,
                      std::size_t selected, bool scanning, bool can_cancel) {
    const char *state_text = scanning ? "正在寻找网络" :
                             count ? "上下选择  确定" :
                             can_cancel ? "等待网络  长按返回"
                                        : "等待网络  长按重试";
    lv_label_set_text(s_wifi_state, state_text);
    lv_obj_set_style_text_color(s_wifi_state,
                                lv_color_hex(scanning ? kAmber : kMuted), 0);
    if (s_wifi_list_hint) {
        lv_label_set_text(s_wifi_list_hint,
                          can_cancel ? "确定设置  长按返回" : "确定后设置网络");
    }

    const std::size_t first = count <= kWifiVisibleRows
                                  ? 0
                                  : std::min(selected > 2 ? selected - 2 : 0,
                                             count - kWifiVisibleRows);
    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const std::size_t index = first + row;
        const bool present = index < count;
        const bool active = present && index == selected;
        lv_obj_set_style_bg_color(s_wifi_row_box[row],
                                  lv_color_hex(active ? kAmberSoft : kPanel), 0);
        if (!present) {
            lv_label_set_text(s_wifi_row_ssid[row], "");
            lv_label_set_text(s_wifi_row_info[row], "");
            continue;
        }

        const uint32_t color = active ? kText : kMuted;
        lv_label_set_text(s_wifi_row_ssid[row], networks[index].ssid);
        lv_obj_set_style_text_font(s_wifi_row_ssid[row],
                                   ssid_font(networks[index].ssid), 0);
        lv_obj_set_style_text_color(s_wifi_row_ssid[row], lv_color_hex(color), 0);
        char info[16];
        std::snprintf(info, sizeof(info), "%s %ddB",
                      networks[index].secure ? "*" : "OPEN",
                      static_cast<int>(networks[index].rssi));
        lv_label_set_text(s_wifi_row_info[row], info);
        lv_obj_set_style_text_color(s_wifi_row_info[row],
                                    lv_color_hex(active ? kAmber : kMuted), 0);
    }
}

void build_wifi_list(const WifiNetworkView *networks, std::size_t count,
                     std::size_t selected, bool scanning, const char *setup_ap_ssid,
                     bool can_cancel) {
    reset_transient_objects(Page::WifiList);
    // The compact UI font contains the complete setup vocabulary. The larger
    // station-title font intentionally contains only station-name glyphs.
    make_label(s_screen, "选择无线网络", 12, 12, 216, kText, &pod_font);
    s_wifi_state = make_label(s_screen, "", 12, 42, 216, kMuted);
    set_centered(s_wifi_state);

    for (std::size_t row = 0; row < kWifiVisibleRows; ++row) {
        const int y = 68 + static_cast<int>(row) * 34;
        s_wifi_row_box[row] = make_box(s_screen, 12, y, 216, 29, kPanel, 5);
        s_wifi_row_ssid[row] = make_label(s_wifi_row_box[row], "", 9, 6, 154,
                                          kMuted, &lv_font_montserrat_14);
        s_wifi_row_info[row] = make_label(s_wifi_row_box[row], "", 158, 7, 50,
                                          kMuted, &lv_font_montserrat_14);
        lv_obj_set_style_text_align(s_wifi_row_info[row], LV_TEXT_ALIGN_RIGHT, 0);
    }

    make_box(s_screen, 12, 277, 216, 1, kGrid);
    s_wifi_list_hint = make_label(s_screen, "", 12, 282, 216, kText);
    set_centered(s_wifi_list_hint);
    char portal[64];
    std::snprintf(portal, sizeof(portal), "WEB: %s",
                  setup_ap_ssid && setup_ap_ssid[0] ? setup_ap_ssid : "Podcast");
    lv_obj_t *hint = make_label(s_screen, portal, 12, 302, 216, kMuted,
                                &lv_font_montserrat_14);
    set_centered(hint);
    update_wifi_list(networks, count, selected, scanning, can_cancel);
}

void update_wifi_keyboard(const char *ssid, std::size_t password_length,
                          WifiKeyboardPage page, std::size_t selected,
                          const char *status) {
    lv_label_set_text(s_wifi_keyboard_network, ssid ? ssid : "WIFI");
    lv_obj_set_style_text_font(s_wifi_keyboard_network, ssid_font(ssid), 0);

    char password[48] = {};
    const std::size_t stars = std::min<std::size_t>(password_length, 38);
    std::memset(password, '*', stars);
    password[stars] = '\0';
    lv_label_set_text(s_wifi_password_mask, password_length ? password : "PASS");
    lv_obj_set_style_text_color(s_wifi_password_mask,
                                lv_color_hex(password_length ? kText : kMuted), 0);

    const std::size_t key_count = podcast_ui_wifi_key_count(page);
    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        if (i >= key_count) {
            lv_obj_add_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_wifi_key_box[i], LV_OBJ_FLAG_HIDDEN);
        const bool active = i == selected;
        lv_obj_set_style_bg_color(s_wifi_key_box[i],
                                  lv_color_hex(active ? kAmber : kPanelSoft), 0);
        lv_label_set_text(s_wifi_key_label[i], podcast_ui_wifi_key(page, i));
        lv_obj_set_style_text_color(s_wifi_key_label[i],
                                    lv_color_hex(active ? kBackground : kText), 0);
    }

    lv_label_set_text(s_wifi_keyboard_status,
                      status && status[0] ? status : "上下选择  确定");
    lv_obj_set_style_text_color(s_wifi_keyboard_status,
                                lv_color_hex(status && status[0] ? kRed : kMuted), 0);
}

void build_wifi_keyboard(const char *ssid, std::size_t password_length,
                         WifiKeyboardPage page, std::size_t selected,
                         const char *status) {
    reset_transient_objects(Page::WifiKeyboard);
    make_label(s_screen, "网络设置", 12, 10, 216, kText, &pod_font);
    s_wifi_keyboard_network = make_label(s_screen, "", 12, 39, 216, kAmber,
                                          &lv_font_montserrat_14);
    set_centered(s_wifi_keyboard_network);

    lv_obj_t *password_box = make_box(s_screen, 18, 66, 204, 34, kPanel, 6);
    s_wifi_password_mask = make_label(password_box, "PASS", 8, 8, 188, kMuted,
                                      &lv_font_montserrat_14);
    set_centered(s_wifi_password_mask);

    for (std::size_t i = 0; i < kWifiMaxKeys; ++i) {
        constexpr int kColumns = 6;
        const int x = 15 + static_cast<int>(i % kColumns) * 35;
        const int y = 109 + static_cast<int>(i / kColumns) * 28;
        // One styled label per key keeps the keyboard within the 24 KB LVGL
        // heap. A separate box + label for every key exhausted that heap near
        // key 31 and caused an immediate Store access fault.
        s_wifi_key_label[i] = make_label(s_screen, "", x, y, 31, kText,
                                         &lv_font_montserrat_14);
        s_wifi_key_box[i] = s_wifi_key_label[i];
        lv_obj_set_size(s_wifi_key_label[i], 31, 24);
        lv_obj_set_style_bg_color(s_wifi_key_label[i], lv_color_hex(kPanelSoft), 0);
        lv_obj_set_style_bg_opa(s_wifi_key_label[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_wifi_key_label[i], 4, 0);
        lv_obj_set_style_pad_top(s_wifi_key_label[i], 4, 0);
        set_centered(s_wifi_key_label[i]);
    }

    make_box(s_screen, 12, 281, 216, 1, kGrid);
    s_wifi_keyboard_status = make_label(s_screen, "", 12, 287, 216, kMuted);
    set_centered(s_wifi_keyboard_status);
    lv_obj_t *hint = make_label(s_screen, "长按上下调节", 12, 304, 216, kMuted);
    set_centered(hint);
    update_wifi_keyboard(ssid, password_length, page, selected, status);
}

void build_wifi_connecting(const char *ssid, const char *detail) {
    reset_transient_objects(Page::WifiConnecting);
    make_label(s_screen, "正在连接无线网络", 12, 37, 216, kText,
               &pod_font);
    lv_obj_t *network = make_label(s_screen, ssid ? ssid : "WIFI", 12, 102, 216,
                                   kAmber, ssid_font(ssid));
    set_centered(network);
    s_pair_dot = make_box(s_screen, 108, 151, 24, 24, kAmber, LV_RADIUS_CIRCLE);
    lv_obj_t *message = make_label(s_screen, detail ? detail : "正在连接",
                                   12, 205, 216, kMuted);
    set_centered(message);
}

void timer_callback(lv_timer_t *) {
    s_animation += 17;
    if (s_page == Page::Config || s_page == Page::WifiConnecting) {
        if (s_pair_dot) {
            const uint8_t triangle = s_animation < 128 ? s_animation : 255 - s_animation;
            lv_obj_set_style_opa(s_pair_dot, 70 + triangle, 0);
        }
        return;
    }

    if (s_clock) {
        std::time_t now = std::time(nullptr);
        if (now > 1700000000) {
            std::tm local_time = {};
            localtime_r(&now, &local_time);
            char text[8];
            std::snprintf(text, sizeof(text), "%02d:%02d", local_time.tm_hour,
                          local_time.tm_min);
            lv_label_set_text(s_clock, text);
        }
    }
    apply_sleep_timer();

    if (++s_battery_ticks >= 300) {
        s_battery_ticks = 0;
        const int soc = bsp_battery_soc();
        if (s_battery && soc >= 0) {
            char text[12];
            std::snprintf(text, sizeof(text), "%d%%", std::clamp(soc, 0, 100));
            lv_label_set_text(s_battery, text);
            lv_obj_set_style_text_color(s_battery,
                                        lv_color_hex(soc <= 15 ? kRed : kMuted), 0);
        }
    }

    const uint32_t generation = s_level_generation.load(std::memory_order_acquire);
    if (generation != s_last_level_generation) {
        s_last_level_generation = generation;
        for (std::size_t i = 0; i < kMeterCount; ++i) {
            if (!s_meter[i]) continue;
            const int height = 3 + s_levels[i] * 34 / 100;
            lv_obj_set_pos(s_meter[i], 9 + static_cast<int>(i) * 11, 40 - height);
            lv_obj_set_size(s_meter[i], 6, height);
            const uint32_t color = s_levels[i] > 85 ? kRed :
                                   s_levels[i] > 62 ? kAmber : kGreen;
            lv_obj_set_style_bg_color(s_meter[i], lv_color_hex(color), 0);
        }
    }
}

}  // namespace

bool podcast_ui_init() {
    if (!bsp_lvgl_lock(1000)) return false;
    // The compact UI font covers interface vocabulary; the GB2312 fallback
    // supplies arbitrary Chinese characters in episode titles.
    lv_font_set_fallback(&pod_font, &pod_font_cjk);
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    build_main();
    lv_screen_load(s_screen);
    lv_timer_create(timer_callback, 50, nullptr);
    bsp_lvgl_unlock();
    return true;
}

void podcast_ui_show_main() {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_main();
    bsp_lvgl_unlock();
}

void podcast_ui_show_config(const char *ssid, const char *url) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_config(ssid, url);
    bsp_lvgl_unlock();
}

void podcast_ui_show_wifi_list(const WifiNetworkView *networks, std::size_t count,
                             std::size_t selected, bool scanning,
                             const char *setup_ap_ssid, bool can_cancel) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::WifiList && s_wifi_state) {
        update_wifi_list(networks, count, selected, scanning, can_cancel);
    } else {
        build_wifi_list(networks, count, selected, scanning, setup_ap_ssid,
                        can_cancel);
    }
    bsp_lvgl_unlock();
}

void podcast_ui_show_wifi_keyboard(const char *ssid, std::size_t password_length,
                                 WifiKeyboardPage page, std::size_t selected,
                                 const char *status) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::WifiKeyboard && s_wifi_password_mask) {
        update_wifi_keyboard(ssid, password_length, page, selected, status);
    } else {
        build_wifi_keyboard(ssid, password_length, page, selected, status);
    }
    bsp_lvgl_unlock();
}

void podcast_ui_show_wifi_connecting(const char *ssid, const char *detail) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_connecting(ssid, detail);
    bsp_lvgl_unlock();
}

const char *podcast_ui_wifi_key(WifiKeyboardPage page, std::size_t index) {
    switch (page) {
        case WifiKeyboardPage::Lower:
            return index < sizeof(kLowerKeys) / sizeof(kLowerKeys[0])
                       ? kLowerKeys[index]
                       : "";
        case WifiKeyboardPage::Upper:
            return index < sizeof(kUpperKeys) / sizeof(kUpperKeys[0])
                       ? kUpperKeys[index]
                       : "";
        case WifiKeyboardPage::Symbols:
            return index < sizeof(kSymbolKeys) / sizeof(kSymbolKeys[0])
                       ? kSymbolKeys[index]
                       : "";
    }
    return "";
}

std::size_t podcast_ui_wifi_key_count(WifiKeyboardPage page) {
    switch (page) {
        case WifiKeyboardPage::Lower: return sizeof(kLowerKeys) / sizeof(kLowerKeys[0]);
        case WifiKeyboardPage::Upper: return sizeof(kUpperKeys) / sizeof(kUpperKeys[0]);
        case WifiKeyboardPage::Symbols:
            return sizeof(kSymbolKeys) / sizeof(kSymbolKeys[0]);
    }
    return 0;
}

void podcast_ui_set_network(bool connected, const char *detail) {
    s_network_connected = connected;
    if (detail) std::snprintf(s_network_detail, sizeof(s_network_detail), "%s", detail);
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_network();
    bsp_lvgl_unlock();
}

void podcast_ui_set_podcast_name(const char *name) {
    std::snprintf(s_podcast_name_text, sizeof(s_podcast_name_text), "%s",
                  name && name[0] ? name : "PODCAST");
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    if (s_podcast_name) lv_label_set_text(s_podcast_name, s_podcast_name_text);
    bsp_lvgl_unlock();
}

void podcast_ui_set_episode(std::size_t index, std::size_t count,
                            const PodcastEpisode &episode) {
    s_episode_index = index;
    s_episode_count = std::max<std::size_t>(count, 1);
    std::snprintf(s_episode_title_text, sizeof(s_episode_title_text), "%s",
                  episode.title);
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_episode();
    bsp_lvgl_unlock();
}

void podcast_ui_set_playback(PodcastPlaybackState state, const char *detail) {
    s_playback_state = state;
    std::snprintf(s_playback_detail, sizeof(s_playback_detail), "%s",
                  detail && detail[0] ? detail : playback_text());
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_playback();
    bsp_lvgl_unlock();
}

void podcast_ui_set_audio_levels(const uint8_t *levels, std::size_t count) {
    if (!levels) return;
    const std::size_t copy_count = std::min(count, kMeterCount);
    std::memcpy(s_levels, levels, copy_count);
    if (copy_count < kMeterCount) {
        std::memset(s_levels + copy_count, 0, kMeterCount - copy_count);
    }
    s_level_generation.fetch_add(1, std::memory_order_release);
}

void podcast_ui_show_settings(PodcastSettingsPage page, std::size_t selected,
                            uint8_t volume) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    if (s_page == Page::Settings && s_settings_page == page &&
        page != PodcastSettingsPage::Volume &&
        page != PodcastSettingsPage::Display && s_settings_row_count > 0) {
        s_settings_selected = selected % s_settings_row_count;
        apply_settings_selection();
    } else {
        build_settings(page, selected, volume);
    }
    bsp_lvgl_unlock();
}

void podcast_ui_show_saved_wifi(const char *const *ssids, std::size_t count,
                              std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_saved_wifi(ssids, count, selected);
    bsp_lvgl_unlock();
}

void podcast_ui_show_wifi_action(const char *ssid, std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_action(ssid, selected);
    bsp_lvgl_unlock();
}

void podcast_ui_show_wifi_delete_confirm(const char *ssid, std::size_t selected) {
    if (!s_screen || !bsp_lvgl_lock(500)) return;
    build_wifi_delete_confirm(ssid, selected);
    bsp_lvgl_unlock();
}

void podcast_ui_set_volume(uint8_t volume) {
    s_volume = volume;
    if (!s_screen || !bsp_lvgl_lock(250)) return;
    apply_volume();
    bsp_lvgl_unlock();
}
