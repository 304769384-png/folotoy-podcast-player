#include "podcast_player.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "podcast_ui.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr char kTag[] = "podcast_player";
constexpr std::size_t kInputSize = 2048;  // larger chunks speed M4A moov intake
constexpr std::size_t kOutputSize = 8192;
constexpr std::size_t kLevelCount = 18;
constexpr std::size_t kMaxEpisodes = 10;
constexpr uint8_t kMaxFailedAttempts = 3;

std::atomic<bool> s_network_connected{false};
std::atomic<bool> s_wanted_playing{true};
std::atomic<std::size_t> s_episode_index{0};
std::atomic<std::size_t> s_episode_count{0};
std::atomic<uint8_t> s_volume{55};
std::atomic<uint32_t> s_generation{1};
std::atomic<bool> s_stream_active{false};
TaskHandle_t s_player_task;
SemaphoreHandle_t s_episode_mutex;
PodcastEpisode s_episodes[kMaxEpisodes] = {};

PodcastEpisode episode_snapshot(std::size_t index) {
    PodcastEpisode episode = {};
    if (!s_episode_mutex ||
        xSemaphoreTake(s_episode_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return episode;
    }
    const std::size_t count = std::max<std::size_t>(s_episode_count.load(), 1);
    episode = s_episodes[index % count];
    xSemaphoreGive(s_episode_mutex);
    return episode;
}

void save_setting_u8(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open("podcast", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

uint8_t load_setting_u8(const char *key, uint8_t fallback) {
    nvs_handle_t handle;
    if (nvs_open("podcast", NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

void save_episode_title(const char *title) {
    nvs_handle_t handle;
    if (nvs_open("podcast", NVS_READWRITE, &handle) != ESP_OK) return;
    if (title && title[0]) {
        nvs_set_str(handle, "ep_title", title);
    } else {
        nvs_erase_key(handle, "ep_title");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

bool load_episode_title(char *title, std::size_t capacity) {
    if (!title || capacity == 0) return false;
    title[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("podcast", NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t length = capacity;
    const esp_err_t error = nvs_get_str(handle, "ep_title", title, &length);
    nvs_close(handle);
    if (error != ESP_OK) {
        title[0] = '\0';
        return false;
    }
    return title[0] != '\0';
}

bool install_episodes(const PodcastEpisode *episodes, std::size_t count,
                      const char *preferred_title, std::size_t *selected) {
    if (!episodes || count == 0 || !s_episode_mutex) return false;
    count = std::min(count, kMaxEpisodes);

    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (xSemaphoreTake(s_episode_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    std::memcpy(s_episodes, episodes, count * sizeof(PodcastEpisode));
    if (count < kMaxEpisodes) {
        std::memset(s_episodes + count, 0,
                    (kMaxEpisodes - count) * sizeof(PodcastEpisode));
    }
    std::size_t index = 0;
    if (preferred_title && preferred_title[0]) {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(s_episodes[i].title, preferred_title) == 0) {
                index = i;
                break;
            }
        }
    }
    s_episode_count.store(count, std::memory_order_release);
    s_episode_index.store(index, std::memory_order_release);
    xSemaphoreGive(s_episode_mutex);
    if (selected) *selected = index;
    return true;
}

bool request_still_current(uint32_t generation, std::size_t episode) {
    return s_network_connected.load(std::memory_order_acquire) &&
           s_wanted_playing.load(std::memory_order_acquire) &&
           s_generation.load(std::memory_order_acq_rel) == generation &&
           s_episode_index.load(std::memory_order_acquire) == episode;
}

void calculate_levels(const uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || bytes < 64) return;
    const auto *samples = reinterpret_cast<const int16_t *>(pcm);
    const std::size_t sample_count = bytes / sizeof(int16_t);
    if (sample_count < kLevelCount) return;
    const std::size_t frames = sample_count / std::max<uint8_t>(channels, 1);
    if (frames < kLevelCount) return;

    uint8_t levels[kLevelCount] = {};
    for (std::size_t band = 0; band < kLevelCount; ++band) {
        const std::size_t begin = band * frames / kLevelCount;
        const std::size_t end = (band + 1) * frames / kLevelCount;
        int64_t energy = 0;
        std::size_t count = 0;
        for (std::size_t frame = begin; frame < end; ++frame) {
            int32_t mixed = 0;
            for (uint8_t channel = 0; channel < channels; ++channel) {
                mixed += samples[frame * channels + channel];
            }
            mixed /= std::max<uint8_t>(channels, 1);
            energy += static_cast<int64_t>(mixed) * mixed;
            ++count;
        }
        const float rms = count ? std::sqrt(static_cast<float>(energy) / count) : 0.0f;
        float db = rms > 1.0f ? 20.0f * std::log10(rms / 32768.0f) : -80.0f;
        int value = static_cast<int>((db + 58.0f) * 2.25f);
        value += static_cast<int>((band * 13 + s_generation.load()) % 9) - 4;
        levels[band] = static_cast<uint8_t>(std::clamp(value, 2, 100));
    }
    podcast_ui_set_audio_levels(levels, kLevelCount);
}

std::size_t downmix_to_mono(uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~static_cast<std::size_t>(1);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const std::size_t frames = bytes / (sizeof(int16_t) * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int32_t mixed = 0;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mixed += samples[frame * channels + channel];
        }
        samples[frame] = static_cast<int16_t>(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

bool ends_with(const char *text, const char *suffix) {
    if (!text || !suffix) return false;
    const std::size_t tl = std::strlen(text);
    const std::size_t sl = std::strlen(suffix);
    return tl >= sl && std::strcmp(text + tl - sl, suffix) == 0;
}

// MP3 files and M4A (AAC-in-MP4) podcast files use different simple decoders.
// The M4A parser demuxes the container internally and emits AAC frames; it only
// supports sequential streaming (no seek), which matches HTTP forward reads.
esp_audio_simple_dec_handle_t open_decoder(const char *url) {
    const bool m4a = ends_with(url, ".m4a") || ends_with(url, ".mp4");
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = m4a ? ESP_AUDIO_SIMPLE_DEC_TYPE_M4A
                        : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const esp_audio_err_t error = esp_audio_simple_dec_open(&config, &decoder);
    if (error != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "%s decoder open failed: %d", m4a ? "M4A" : "MP3", error);
        return nullptr;
    }
    return decoder;
}

// Streams one episode. `completed` is set true only when audio played and the
// file reached a natural end of stream (podcast finished, advance to next).
// Turn a possibly-relative HTTP "Location" header into an absolute URL.
// Some podcast CDNs (e.g. Fireside) hand back a relative path on redirect.
char *resolve_url(const char *base, const char *location) {
    if (std::strncmp(location, "http://", 7) == 0 ||
        std::strncmp(location, "https://", 8) == 0) {
        return strdup(location);
    }
    const char *scheme_end = std::strstr(base, "://");
    if (!scheme_end) return strdup(location);
    const char *host_start = scheme_end + 3;
    const char *path_start = std::strchr(host_start, '/');
    const std::string scheme(base, scheme_end - base);
    const std::string authority =
        path_start ? std::string(host_start, path_start)
                   : std::string(host_start);
    if (location[0] == '/') {
        return strdup((scheme + "//" + authority + location).c_str());
    }
    std::string base_path =
        path_start ? std::string(path_start) : std::string("/");
    const std::string::size_type slash = base_path.find_last_of('/');
    if (slash != std::string::npos) base_path.resize(slash + 1);
    return strdup((scheme + "//" + authority + base_path + location).c_str());
}

bool stream_episode(std::size_t episode, uint32_t generation, bool *completed) {
    if (completed) *completed = false;
    const PodcastEpisode preset = episode_snapshot(episode);
    if (!preset.url[0]) {
        ESP_LOGE(kTag, "Episode %u has no URL", static_cast<unsigned>(episode));
        return false;
    }
    podcast_ui_set_playback(PodcastPlaybackState::Connecting, "正在连接节目");

    esp_http_client_config_t config = {};
    config.url = preset.url;
    config.timeout_ms = 10000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 512;
    config.user_agent = "AI-Passport-Podcast/0.1";
    config.keep_alive_enable = true;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 4;
    if (std::strncmp(preset.url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = nullptr;
    char *request_url = strdup(preset.url);

    bool played_audio = false;
    bool clean_eof = false;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    uint8_t *input = nullptr;
    uint8_t *output = nullptr;

    do {
        // Every audio source here serves its file behind a 3xx redirect (the
        // xiaoyuzhou dts, Fireside and Ximalaya links all 302 to the real CDN).
        // The streaming open()->read() flow does not chase redirects itself,
        // so resolve up to 4 hops by hand before decoding.
        bool resolved = false;
        for (int hop = 0; hop < 4 && !resolved; ++hop) {
            if (client) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                client = nullptr;
            }
            config.url = request_url;
            client = esp_http_client_init(&config);
            if (!client) {
                ESP_LOGE(kTag, "HTTP client allocation failed");
                break;
            }
            if (esp_http_client_open(client, 0) != ESP_OK) {
                ESP_LOGW(kTag, "Open failed: %s", request_url);
                break;
            }
            esp_http_client_fetch_headers(client);
            const int status = esp_http_client_get_status_code(client);
            if (status >= 300 && status < 400) {
                const char *location = nullptr;
                if (esp_http_client_get_header(client, "Location", &location) !=
                        ESP_OK ||
                    location == nullptr || location[0] == '\0') {
                    ESP_LOGW(kTag, "HTTP redirect %d without Location", status);
                    break;
                }
                char *next = resolve_url(request_url, location);
                std::free(request_url);
                request_url = next;
                continue;  // follow the redirect on the next hop
            }
            if (status < 200 || status >= 300) {
                ESP_LOGW(kTag, "HTTP status %d", status);
                break;
            }
            resolved = true;
        }
        if (!resolved) break;

        esp_http_client_set_header(client, "Accept", "audio/mp4,audio/mpeg,*/*");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");

        input = static_cast<uint8_t *>(malloc(kInputSize));
        output = static_cast<uint8_t *>(malloc(kOutputSize));
        if (!input || !output) {
            ESP_LOGE(kTag, "Not enough memory for stream buffers");
            break;
        }

        podcast_ui_set_playback(PodcastPlaybackState::Buffering, "正在缓冲");
        bool format_ready = false;
        uint8_t source_channels = 1;
        int empty_reads = 0;
        while (request_still_current(generation, episode)) {
            const int received = esp_http_client_read(
                client, reinterpret_cast<char *>(input), kInputSize);
            if (received < 0) {
                ESP_LOGW(kTag, "Stream read error");
                break;
            }
            if (received == 0) {
                if (++empty_reads > 3) {
                    clean_eof = played_audio;  // natural end after playback
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            empty_reads = 0;

            // The decoder is opened lazily so the HTTP error path never pays
            // for it. Feed the first network block onward to the parser.
            if (!decoder) {
                decoder = open_decoder(preset.url);
                if (!decoder) break;
            }

            esp_audio_simple_dec_raw_t raw = {
                .buffer = input,
                .len = static_cast<uint32_t>(received),
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            while (raw.len > 0 && request_still_current(generation, episode)) {
                esp_audio_simple_dec_out_t frame = {
                    .buffer = output,
                    .len = kOutputSize,
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                const uint32_t before = raw.len;
                const esp_audio_err_t result =
                    esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    ESP_LOGE(kTag, "Decoder needs %u bytes, buffer has %u",
                             static_cast<unsigned>(frame.needed_size),
                             static_cast<unsigned>(kOutputSize));
                    raw.len = 0;
                    break;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(kTag, "Decode failed: %d", result);
                    raw.len = 0;
                    break;
                }
                if (raw.consumed > raw.len) raw.consumed = raw.len;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {};
                    if (!format_ready &&
                        esp_audio_simple_dec_get_info(decoder, &info) ==
                            ESP_AUDIO_ERR_OK) {
                        source_channels = std::clamp<uint8_t>(info.channel, 1, 2);
                        if (info.bits_per_sample != 16 ||
                            info.sample_rate < 8000 ||
                            info.sample_rate > 48000 ||
                            bsp_audio_set_format(info.sample_rate, 16, 1) !=
                                ESP_OK) {
                            ESP_LOGE(kTag, "Unsupported format: %luHz/%ubit/%uch",
                                     static_cast<unsigned long>(info.sample_rate),
                                     info.bits_per_sample, source_channels);
                            raw.len = 0;
                            break;
                        }
                        bsp_audio_set_volume(s_volume.load(std::memory_order_acquire));
                        format_ready = true;
                        ESP_LOGI(kTag, "Playing %s at %luHz/%uch -> mono",
                                 preset.title,
                                 static_cast<unsigned long>(info.sample_rate),
                                 source_channels);
                    }
                    if (format_ready) {
                        const std::size_t mono_bytes =
                            downmix_to_mono(frame.buffer, frame.decoded_size,
                                            source_channels);
                        if (bsp_audio_write(frame.buffer, mono_bytes) != ESP_OK)
                            continue;
                        calculate_levels(frame.buffer, mono_bytes, 1);
                        if (!played_audio) {
                            played_audio = true;
                            podcast_ui_set_playback(PodcastPlaybackState::Playing,
                                                    "正在播放");
                        }
                    }
                }

                if (raw.len == before || raw.consumed == 0) {
                    // Parser cached this block (typical while ingesting the
                    // M4A moov atom); fetch a fresh block instead of spinning.
                    break;
                }
            }
        }
    } while (false);

    if (decoder) esp_audio_simple_dec_close(decoder);
    free(output);
    free(input);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    std::free(request_url);
    if (completed) *completed = clean_eof;
    return played_audio;
}

void player_task(void *) {
    std::size_t last_attempted = SIZE_MAX;
    uint8_t failed_attempts = 0;
    while (true) {
        if (!s_network_connected.load(std::memory_order_acquire)) {
            podcast_ui_set_playback(PodcastPlaybackState::Stopped, "等待网络");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!s_wanted_playing.load(std::memory_order_acquire)) {
            podcast_ui_set_playback(PodcastPlaybackState::Stopped, "已暂停");
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        if (s_episode_count.load(std::memory_order_acquire) == 0) {
            podcast_ui_set_playback(PodcastPlaybackState::Stopped, "暂无节目");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        const std::size_t episode = s_episode_index.load(std::memory_order_acquire);
        const uint32_t generation = s_generation.load(std::memory_order_acq_rel);
        if (episode != last_attempted) {
            last_attempted = episode;
            failed_attempts = 0;
        }
        s_stream_active.store(true, std::memory_order_release);
        bool completed = false;
        const bool played = stream_episode(episode, generation, &completed);
        s_stream_active.store(false, std::memory_order_release);
        if (!request_still_current(generation, episode)) continue;

        if (completed) {
            // Finished this episode naturally: move on (wraps around).
            failed_attempts = 0;
            podcast_player_select_relative(1);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        if (played) {
            failed_attempts = 0;
            podcast_ui_set_playback(PodcastPlaybackState::Reconnecting,
                                    "信号中断，正在重连");
            vTaskDelay(pdMS_TO_TICKS(1200));
            continue;
        }

        ++failed_attempts;
        const std::size_t count = podcast_player_count();
        if (failed_attempts >= kMaxFailedAttempts && count > 1) {
            ESP_LOGW(kTag, "Episode %u unreachable, skipping",
                     static_cast<unsigned>(episode));
            podcast_ui_set_playback(PodcastPlaybackState::Error,
                                    "节目无法播放 已换下一期");
            vTaskDelay(pdMS_TO_TICKS(1200));
            failed_attempts = 0;
            podcast_player_select_relative(1);
            continue;
        }
        podcast_ui_set_playback(PodcastPlaybackState::Error, "连接失败，正在重试");
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

}  // namespace

bool podcast_player_init() {
    s_episode_mutex = xSemaphoreCreateMutex();
    if (!s_episode_mutex) return false;

    const uint8_t saved_episode = load_setting_u8("episode", 0);
    const uint8_t saved_volume = load_setting_u8("volume", 55);
    s_volume.store(std::clamp<uint8_t>(saved_volume, 10, 100));

    if (bsp_audio_init() != ESP_OK) return false;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    bsp_audio_set_volume(s_volume.load());

    podcast_ui_set_volume(s_volume.load());
    if (xTaskCreate(player_task, "podcast_stream", 8192, nullptr, 6,
                    &s_player_task) != pdPASS) {
        s_player_task = nullptr;
        return false;
    }
    return true;
}

void podcast_player_set_network(bool connected) {
    s_network_connected.store(connected, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

void podcast_player_toggle() {
    podcast_player_set_playing(!podcast_player_is_playing());
}

void podcast_player_set_playing(bool playing) {
    s_wanted_playing.store(playing, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (!playing) podcast_ui_set_playback(PodcastPlaybackState::Stopped, "已暂停");
}

bool podcast_player_is_playing() {
    return s_wanted_playing.load(std::memory_order_acquire);
}

void podcast_player_select_relative(int delta) {
    const int count = static_cast<int>(podcast_player_count());
    const int current = static_cast<int>(s_episode_index.load());
    const std::size_t next =
        static_cast<std::size_t>((current + delta + count) % count);
    podcast_player_set_episode(next);
}

void podcast_player_set_episode(std::size_t index) {
    index %= podcast_player_count();
    s_episode_index.store(index, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    save_setting_u8("episode", static_cast<uint8_t>(index));
    const PodcastEpisode chosen = podcast_player_episode(index);
    save_episode_title(chosen.title);
    podcast_ui_set_episode(index, podcast_player_count(), chosen);
    if (podcast_player_is_playing()) {
        podcast_ui_set_playback(PodcastPlaybackState::Connecting, "正在切换节目");
    }
}

std::size_t podcast_player_index() {
    return s_episode_index.load(std::memory_order_acquire);
}

std::size_t podcast_player_count() {
    return std::max<std::size_t>(s_episode_count.load(std::memory_order_acquire), 1);
}

PodcastEpisode podcast_player_episode(std::size_t index) {
    return episode_snapshot(index);
}

bool podcast_player_replace_episodes(const PodcastEpisode *episodes,
                                     std::size_t count) {
    char remembered[sizeof(PodcastEpisode::title)] = {};
    const bool has_remembered = load_episode_title(remembered, sizeof(remembered));

    std::size_t selected = 0;
    if (!install_episodes(episodes, count, has_remembered ? remembered : nullptr,
                          &selected)) {
        return false;
    }

    save_setting_u8("episode", static_cast<uint8_t>(selected));
    const PodcastEpisode chosen = podcast_player_episode(selected);
    save_episode_title(chosen.title);
    podcast_ui_set_episode(selected, podcast_player_count(), chosen);
    ESP_LOGI(kTag, "Installed %u episodes", static_cast<unsigned>(count));
    return true;
}

void podcast_player_set_volume(uint8_t volume) {
    volume = std::clamp<uint8_t>(volume, 10, 100);
    s_volume.store(volume, std::memory_order_release);
    bsp_audio_set_volume(volume);
    save_setting_u8("volume", volume);
    podcast_ui_set_volume(volume);
}

uint8_t podcast_player_volume() {
    return s_volume.load(std::memory_order_acquire);
}
