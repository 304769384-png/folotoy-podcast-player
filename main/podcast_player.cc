#include "podcast_player.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
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
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

#include <cstdarg>
#include <cstdio>

namespace {

constexpr char kTag[] = "podcast_player";
// Larger network read chunks amortise TLS/HTTP overhead, which is what makes
// both the initial fill and the steady stream faster (the old 2 KB reads
// caused long "正在缓冲" and starved the tiny I2S DMA ring).
constexpr std::size_t kInputSize = 8192;
constexpr std::size_t kOutputSize = 8192;
constexpr std::size_t kLevelCount = 18;
constexpr std::size_t kMaxEpisodes = 10;
constexpr uint8_t kMaxFailedAttempts = 3;

// ---- Buffering architecture (two-task produce/consume) ----
// Playback is split into two tasks. A "producer" task reads + decodes the
// network stream into a software ring (it keeps running ahead even while
// paused); a "consumer" task on player_task drains the surplus to the I2S
// speaker at real time. The previous single-threaded loop called the blocking
// bsp_audio_write() (which is paced by real time) in the same iteration as the
// network read, so any slow read starved the ~32 ms I2S DMA ring and produced
// a "stalled second for every played second" stutter. Splitting the tasks
// means the speaker keeps playing from the in-RAM cushion while a slow read
// fills behind it, and pausing only halts the consumer so the buffer keeps
// topping up (resume is instant instead of a full re-buffer).
// Ring is sized adaptively at run time (choose_ring_size) to fit the tight
// ESP32-C3 heap. The old fixed 64 KB ring pushed peak usage into an
// allocation failure ("E4 mem") once WiFi/TLS was up, even though the
// producer's own input/output buffers and the HTTP/TLS working set are only
// ~32 KB. These are the bounds the ring may take and the cushion logic uses.
constexpr std::size_t kPcmRingSizeMax = 32768;   // ~0.37 s @44.1 kHz mono
constexpr std::size_t kPcmRingSizeMin = 16384;   // floor we never dip under
constexpr std::size_t kPcmCushion = 8192;         // keep this slack while playing
constexpr std::size_t kDrainChunk = 2048;

// Buffering watchdog bounds (ms). Some podcast CDNs (e.g. Fireside/hosting
// CDNs behind two 3xx hops) are slow to first byte, so the "no audio at all"
// budget is generous; but once audio is being decoded into the ring we require
// steady progress so a starved/trickling stream fails fast instead of showing
// "正在缓冲" forever.
constexpr uint32_t kBufferingNoDataTimeoutMs = 12000;
constexpr uint32_t kBufferingStallTimeoutMs = 6000;


// Simple single-producer / single-consumer ring buffer for decoded PCM.
struct PcmRing {
    uint8_t *buf = nullptr;
    std::size_t size = 0;
    std::size_t head = 0;
    std::size_t used = 0;

    explicit PcmRing(std::size_t bytes) : size(bytes) {
        buf = static_cast<uint8_t *>(malloc(bytes));
    }
    ~PcmRing() { std::free(buf); }
    PcmRing(const PcmRing &) = delete;
    PcmRing &operator=(const PcmRing &) = delete;

    std::size_t used_bytes() const { return used; }
    std::size_t free_space() const { return size - used; }

    std::size_t write(const uint8_t *data, std::size_t n) {
        const std::size_t space = size - used;
        n = std::min(n, space);
        if (n == 0) return 0;
        const std::size_t pos = (head + used) % size;
        const std::size_t first = std::min(n, size - pos);
        std::memcpy(buf + pos, data, first);
        if (first < n) std::memcpy(buf, data + first, n - first);
        used += n;
        return n;
    }

    std::size_t read(uint8_t *out, std::size_t n) {
        n = std::min(n, used);
        if (n == 0) return 0;
        const std::size_t first = std::min(n, size - head);
        std::memcpy(out, buf + head, first);
        if (first < n) std::memcpy(out + first, buf, n - first);
        head = (head + n) % size;
        used -= n;
        return n;
    }
};

// Shared state between the producer and consumer tasks of one stream.
struct StreamCtx {
    PcmRing *pcm = nullptr;
    SemaphoreHandle_t mutex = nullptr;    // guards the ring (single prod/consumer)
    SemaphoreHandle_t data_new = nullptr; // counting: signalled when audio is added
    std::atomic<bool> eof{false};         // producer reached network end of stream
    std::atomic<bool> format_ready{false};// codec format configured + volume set
    std::atomic<bool> stop{false};        // consumer asked the producer to wind down
    std::atomic<bool> producer_done{false};
    std::atomic<esp_http_client_handle_t> client{nullptr};
    // Watchdog anchors: started_tick is stamped when the consumer begins
    // buffering; last_data_tick is refreshed whenever decoded PCM reaches the
    // ring. Together they bound "正在缓冲" so a trickling or stalled CDN can
    // never hold the player there forever (the slow-TTFB fixes at the read
    // layer are useless if the UI never gives up and retries).
    std::atomic<uint32_t> started_tick{0};
    std::atomic<uint32_t> last_data_tick{0};
    uint8_t channels = 1;
};

// Thread-safe accessors over the shared ring (only touched by the two tasks).
std::size_t ring_used(StreamCtx &ctx) {
    if (!ctx.mutex || !ctx.pcm) return 0;
    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(60)) != pdTRUE) return 0;
    const std::size_t used = ctx.pcm->used_bytes();
    xSemaphoreGive(ctx.mutex);
    return used;
}

std::size_t ring_write(StreamCtx &ctx, const uint8_t *data, std::size_t n) {
    if (!ctx.mutex || !ctx.pcm) return 0;
    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(60)) != pdTRUE) return 0;
    const std::size_t wrote = ctx.pcm->write(data, n);
    xSemaphoreGive(ctx.mutex);
    return wrote;
}

std::size_t ring_read(StreamCtx &ctx, uint8_t *out, std::size_t n) {
    if (!ctx.mutex || !ctx.pcm) return 0;
    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(60)) != pdTRUE) return 0;
    const std::size_t got = ctx.pcm->read(out, n);
    xSemaphoreGive(ctx.mutex);
    return got;
}

std::size_t ring_space(StreamCtx &ctx) {
    if (!ctx.mutex || !ctx.pcm) return 0;
    if (xSemaphoreTake(ctx.mutex, pdMS_TO_TICKS(60)) != pdTRUE) return 0;
    const std::size_t space = ctx.pcm->free_space();
    xSemaphoreGive(ctx.mutex);
    return space;
}

// Holds the most recent playback failure reason. Surfaced on the display so a
// failed episode tells us exactly which stage died instead of a generic
// "连接失败" (HTTP open, redirect, status, buffer, decoder, format...).
char s_last_fail[96];

void set_fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(s_last_fail, sizeof(s_last_fail), fmt, args);
    va_end(args);
}

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

// Streaming continuity for inside stream_episode: keep the live connection and
// decoder alive as long as the same episode is selected and the network is up.
// Deliberately does NOT check s_wanted_playing or generation, so pausing only
// stops the speaker (handled in the loop) without tearing down the stream,
// which makes resume instant instead of a full re-download + re-buffer.
bool stream_alive(std::size_t episode) {
    return s_network_connected.load(std::memory_order_acquire) &&
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

// esp_http_client only auto-follows 3xx inside the blocking perform() path;
// the streaming open()->read() flow never does, so we resolve the hops by
// hand. The handle is an opaque type, so we cannot read its parsed response
// headers directly. The one public way to see a *response* header is the
// HTTP_EVENT_ON_HEADER event, which carries each header key/value pair.
// We capture the redirect's Location there so the manual follower can build
// the next URL.
struct RedirectCtx {
    bool has_location = false;
    char location[512];
};

esp_err_t redirect_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        evt->header_key != nullptr && evt->header_value != nullptr &&
        strcasecmp(evt->header_key, "Location") == 0) {
        RedirectCtx *ctx = static_cast<RedirectCtx *>(evt->user_data);
        if (ctx != nullptr) {
            std::strncpy(ctx->location, evt->header_value,
                         sizeof(ctx->location) - 1);
            ctx->location[sizeof(ctx->location) - 1] = '\0';
            ctx->has_location = true;
        }
    }
    return ESP_OK;
}

struct ProducerArg {
    StreamCtx *ctx = nullptr;
    const char *url = nullptr;
    std::size_t episode = 0;
};

// Producer task: owns the HTTP connection + decoder, reads compressed bytes,
// decodes them into the shared ring, and signals the consumer. It deliberately
// does NOT touch the speaker and deliberately does NOT stop when paused, so it
// keeps topping up the ring while the user pauses (resume is instant) and is
// never blocked by the real-time drain (no per-read stutter).
void stream_producer(void *arg_ptr) {
    ProducerArg *arg = static_cast<ProducerArg *>(arg_ptr);
    StreamCtx &ctx = *arg->ctx;
    const char *preset_url = arg->url;
    const std::size_t episode = arg->episode;

    uint8_t *input = static_cast<uint8_t *>(malloc(kInputSize));
    uint8_t *output = static_cast<uint8_t *>(malloc(kOutputSize));
    if (!input || !output) {
        set_fail("E4 mem");
        ctx.eof.store(true, std::memory_order_release);
        std::free(input);
        std::free(output);
        ctx.producer_done.store(true, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }

    char *request_url = strdup(preset_url);
    esp_http_client_handle_t client = nullptr;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    RedirectCtx redirect_ctx{};
    esp_http_client_config_t config = {};
    config.url = preset_url;
    config.timeout_ms = 20000;
    config.buffer_size = 8192;
    config.buffer_size_tx = 1024;
    config.user_agent = "AI-Passport-Podcast/0.1";
    config.keep_alive_enable = false;
    config.disable_auto_redirect = true;
    config.max_redirection_count = 0;
    config.event_handler = redirect_event_handler;
    config.user_data = &redirect_ctx;
    if (std::strncmp(preset_url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    // Follow up to 4 manual redirect hops (streaming read() never auto-follows).
    bool run = true;
    bool resolved = false;
    for (int hop = 0; hop < 4 && !resolved && run; ++hop) {
        if (client) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
        }
        config.url = request_url;
        redirect_ctx.has_location = false;
        redirect_ctx.location[0] = '\0';
        client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(kTag, "HTTP client allocation failed");
            set_fail("E1 alloc");
            run = false;
            break;
        }
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(kTag, "Open failed: %s", request_url);
            set_fail("E1 open fail");
            run = false;
            break;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status >= 300 && status < 400) {
            if (!redirect_ctx.has_location || redirect_ctx.location[0] == '\0') {
                ESP_LOGW(kTag, "HTTP redirect %d without Location", status);
                set_fail("E2 redirect no loc");
                run = false;
                break;
            }
            char *next = resolve_url(request_url, redirect_ctx.location);
            std::free(request_url);
            request_url = next;
            continue;
        }
        if (status < 200 || status >= 300) {
            ESP_LOGW(kTag, "HTTP status %d", status);
            set_fail("E3 http %d", status);
            run = false;
            break;
        }
        resolved = true;
    }
    if (run && !resolved) run = false;

    if (run) {
        ctx.client.store(client, std::memory_order_release);
        esp_http_client_set_header(client, "Accept", "audio/mp4,audio/mpeg,*/*");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");

        bool stream_eof = false;
        int empty_reads = 0;
        while (stream_alive(episode) && !ctx.stop.load(std::memory_order_acquire)) {
            if (ctx.format_ready.load(std::memory_order_acquire) &&
                ring_space(ctx) == 0) {
                // The consumer cannot keep up (e.g. the user is paused and the
                // ring is fully topped up): backpressure here instead of
                // feeding the decoder cache unboundedly while nothing drains.
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const int received = esp_http_client_read(
                client, reinterpret_cast<char *>(input), kInputSize);
            if (received < 0) {
                ESP_LOGW(kTag, "Stream read error %d (errno %d)",
                         received, errno);
                set_fail("E5 read err");
                break;
            }
            if (received == 0) {
                if (++empty_reads > 3) {
                    stream_eof = true;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
            }
            empty_reads = 0;

            if (!decoder) {
                decoder = open_decoder(preset_url);
                if (!decoder) {
                    set_fail("E6 dec open");
                    run = false;
                    break;
                }
            }

            if (received > 0) {
                esp_audio_simple_dec_raw_t raw = {
                    .buffer = input,
                    .len = static_cast<uint32_t>(received),
                    .eos = false,
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
                };
                while (raw.len > 0 && stream_alive(episode) &&
                       !ctx.stop.load(std::memory_order_acquire)) {
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
                        set_fail("E7 buf %u/%u",
                                 static_cast<unsigned>(frame.needed_size),
                                 static_cast<unsigned>(kOutputSize));
                        raw.len = 0;
                        break;
                    }
                    if (result != ESP_AUDIO_ERR_OK) {
                        ESP_LOGW(kTag, "Decode failed: %d", result);
                        set_fail("E8 dec %d", static_cast<int>(result));
                        raw.len = 0;
                        break;
                    }
                    if (raw.consumed > raw.len) raw.consumed = raw.len;
                    raw.buffer += raw.consumed;
                    raw.len -= raw.consumed;

                    if (frame.decoded_size > 0) {
                        esp_audio_simple_dec_info_t info = {};
                        if (!ctx.format_ready.load(std::memory_order_acquire) &&
                            esp_audio_simple_dec_get_info(decoder, &info) ==
                                ESP_AUDIO_ERR_OK) {
                            ctx.channels = std::clamp<uint8_t>(info.channel, 1, 2);
                            if (info.bits_per_sample != 16 ||
                                info.sample_rate < 8000 ||
                                info.sample_rate > 48000 ||
                                bsp_audio_set_format(info.sample_rate, 16, 1) !=
                                    ESP_OK) {
                                ESP_LOGE(kTag,
                                         "Unsupported format: %luHz/%ubit/%uch",
                                         static_cast<unsigned long>(
                                             info.sample_rate),
                                         info.bits_per_sample, ctx.channels);
                                set_fail("E9 fmt %lu",
                                         static_cast<unsigned long>(
                                             info.sample_rate));
                                raw.len = 0;
                                break;
                            }
                            bsp_audio_set_volume(
                                s_volume.load(std::memory_order_acquire));
                            ctx.format_ready.store(true, std::memory_order_release);
                            ctx.last_data_tick.store(xTaskGetTickCount(),
                                                     std::memory_order_release);
                            const PodcastEpisode preset = episode_snapshot(episode);
                            ESP_LOGI(kTag, "Playing %s at %luHz/%uch -> mono",
                                     preset.title,
                                     static_cast<unsigned long>(info.sample_rate),
                                     ctx.channels);
                        }
                        if (ctx.format_ready.load(std::memory_order_acquire)) {
                            const std::size_t mono_bytes =
                                downmix_to_mono(frame.buffer, frame.decoded_size,
                                                ctx.channels);
                            // Backpressure when the ring is full so we never
                            // overwrite audio the consumer has not played yet.
                            for (int tries = 0;
                                 tries < 50 &&
                                 ring_space(ctx) < mono_bytes;
                                 ++tries) {
                                vTaskDelay(pdMS_TO_TICKS(2));
                            }
                            if (ring_write(ctx, frame.buffer, mono_bytes) > 0) {
                                ctx.last_data_tick.store(
                                    xTaskGetTickCount(),
                                    std::memory_order_release);
                                xSemaphoreGive(ctx.data_new);
                            }
                        }
                    }

                    if (raw.len == before || raw.consumed == 0) {
                        // Parser cached this block; fetch a fresh block instead
                        // of spinning.
                        break;
                    }
                }
            }
            if (stream_eof) break;
        }
    }

    ctx.eof.store(true, std::memory_order_release);
    ctx.client.store(nullptr, std::memory_order_release);
    xSemaphoreGive(ctx.data_new);  // nudge a waiting consumer to check eof
    if (decoder) esp_audio_simple_dec_close(decoder);
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    std::free(request_url);
    std::free(input);
    std::free(output);
    ctx.producer_done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

// Pick the decoded-PCM ring size that fits this moment's heap. On ESP32-C3 the
// free pool is a moving target once WiFi/TLS/decoder are resident, so a fixed
// 64 KB ring could exceed it and fail ("E4 mem"). We keep a firewall of free
// heap for the decoder, the HTTPS handshake and the producer's own
// input/output + HTTP buffers (roughly 32 KB all together), then take the
// largest slice of what remains, rounded down to a 4 KB multiple and clamped
// to [min, max]. Driving the split from free heap instead of a constant is
// what lets the same dual-task architecture survive low-memory moments.
std::size_t choose_ring_size() {
    constexpr std::size_t kReserve = 32768;
    const std::size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    std::size_t ring = kPcmRingSizeMax;
    if (free_heap > kReserve) {
        ring = std::min(kPcmRingSizeMax, free_heap - kReserve);
    }
    ring &= ~static_cast<std::size_t>(0xfff);  // round down to a 4 KB slice
    ring = std::max(kPcmRingSizeMin, ring);
    ESP_LOGI(kTag, "Free heap %u -> PCM ring %u",
             static_cast<unsigned>(free_heap), static_cast<unsigned>(ring));
    return ring;
}

bool stream_episode(std::size_t episode, uint32_t generation, bool *completed) {
    if (completed) *completed = false;
    const PodcastEpisode preset = episode_snapshot(episode);
    if (!preset.url[0]) {
        ESP_LOGE(kTag, "Episode %u has no URL", static_cast<unsigned>(episode));
        return false;
    }
    podcast_ui_set_playback(PodcastPlaybackState::Connecting, "正在连接节目");

    // -------- consumer task (runs on player_task) --------
    // It only drains surplus audio from the ring to the speaker when playing;
    // the producer task fills the ring independently, which is what decouples
    // network reads from real-time output.
    const std::size_t ring_size = choose_ring_size();
    const std::size_t start_threshold = ring_size / 2;  // pre-roll cushion
    // Keep a small slack so the drain loop doesn't spin on every byte; it is
    // deliberately modest so a small ring still leaves most of it to play.
    const std::size_t cushion = std::min<std::size_t>(ring_size / 8, 4096);
    PcmRing pcm(ring_size);
    if (!pcm.buf) {
        podcast_ui_set_playback(PodcastPlaybackState::Error, "缓冲内存不足");
        return false;
    }
    StreamCtx ctx;
    ctx.pcm = &pcm;
    ctx.mutex = xSemaphoreCreateMutex();
    ctx.data_new = xSemaphoreCreateCounting(8, 0);
    if (!ctx.mutex || !ctx.data_new) {
        if (ctx.mutex) vSemaphoreDelete(ctx.mutex);
        if (ctx.data_new) vSemaphoreDelete(ctx.data_new);
        podcast_ui_set_playback(PodcastPlaybackState::Error, "信号量创建失败");
        return false;
    }

    ProducerArg arg;
    arg.ctx = &ctx;
    arg.url = preset.url;
    arg.episode = episode;

    podcast_ui_set_playback(PodcastPlaybackState::Buffering, "正在缓冲");
    ctx.started_tick.store(xTaskGetTickCount(), std::memory_order_release);
    TaskHandle_t producer = nullptr;
    if (xTaskCreate(stream_producer, "podcast_prod", 8192, &arg, 5,
                    &producer) != pdPASS) {
        vSemaphoreDelete(ctx.data_new);
        vSemaphoreDelete(ctx.mutex);
        podcast_ui_set_playback(PodcastPlaybackState::Error, "任务创建失败");
        return false;
    }

    uint8_t *drain = static_cast<uint8_t *>(malloc(kDrainChunk));
    if (!drain) {
        ctx.stop.store(true, std::memory_order_release);
        for (int i = 0; i < 60 && !ctx.producer_done.load(std::memory_order_acquire);
             ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vSemaphoreDelete(ctx.data_new);
        vSemaphoreDelete(ctx.mutex);
        podcast_ui_set_playback(PodcastPlaybackState::Error, "缓冲内存不足");
        return false;
    }

    bool started = false;
    bool played_audio = false;
    bool clean_eof = false;

    while (stream_alive(episode)) {
        // ---- pause: hold the stream; only stop draining. The producer keeps
        // topping up the ring, so resume drains instantly (no re-buffer) ----
        if (!s_wanted_playing.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        // ---- buffering watchdog: never leave "正在缓冲" spinning ----
        if (!started &&
            s_wanted_playing.load(std::memory_order_acquire)) {
            const uint32_t now = xTaskGetTickCount();
            const uint32_t last = ctx.last_data_tick.load(
                std::memory_order_acquire);
            const bool stall =
                (last != 0)
                    ? (now - last) >
                          pdMS_TO_TICKS(kBufferingStallTimeoutMs)
                    : (now - ctx.started_tick.load(
                                 std::memory_order_acquire)) >
                          pdMS_TO_TICKS(kBufferingNoDataTimeoutMs);
            if (stall) {
                ESP_LOGW(kTag, "Buffering stalled (no %s), retrying",
                         last ? "new audio" : "audio at all");
                set_fail("E9 stall");
                break;
            }
        }

        // ---- wait for the codec format before writing anything ----
        if (!ctx.format_ready.load(std::memory_order_acquire)) {
            if (ctx.eof.load(std::memory_order_acquire) &&
                ring_used(ctx) == 0) {
                break;  // stream ended before any format was negotiated
            }
            xSemaphoreTake(ctx.data_new, pdMS_TO_TICKS(120));
            continue;
        }

        // ---- pre-roll: start playing only once a cushion is buffered ----
        if (!started) {
            if (ring_used(ctx) >= start_threshold) {
                started = true;
                if (!played_audio) {
                    podcast_ui_set_playback(PodcastPlaybackState::Playing,
                                            "正在播放");
                }
            } else {
                if (ctx.eof.load(std::memory_order_acquire) &&
                    ring_used(ctx) == 0) {
                    break;
                }
                xSemaphoreTake(ctx.data_new, pdMS_TO_TICKS(100));
                continue;
            }
        }

        // ---- consume: drain the surplus to the speaker ----
        bool drained_any = false;
        while (started && ring_used(ctx) > cushion) {
            const std::size_t take = std::min<std::size_t>(
                ring_used(ctx) - cushion, kDrainChunk);
            const std::size_t got = ring_read(ctx, drain, take);
            if (got == 0) break;
            if (bsp_audio_write(drain, got) != ESP_OK) break;
            calculate_levels(drain, got, 1);
            played_audio = true;
            drained_any = true;
        }

        if (!drained_any) {
            if (ctx.eof.load(std::memory_order_acquire)) {
                // Network ended: drain whatever remains into the speaker as the
                // tail, then finish. Drains even if the pre-roll threshold was
                // never reached, so a short/ending stream still plays out.
                started = true;
                while (ring_used(ctx) > 0) {
                    const std::size_t take = std::min<std::size_t>(
                        ring_used(ctx), kDrainChunk);
                    const std::size_t got = ring_read(ctx, drain, take);
                    if (got == 0) break;
                    if (bsp_audio_write(drain, got) != ESP_OK) break;
                    calculate_levels(drain, got, 1);
                    played_audio = true;
                }
                if (ring_used(ctx) == 0) {
                    if (played_audio) {
                        podcast_ui_set_playback(PodcastPlaybackState::Playing,
                                                "正在播放");
                    }
                    clean_eof = played_audio;  // natural end after playback
                    break;
                }
            } else {
                xSemaphoreTake(ctx.data_new, pdMS_TO_TICKS(120));
            }
        }
    }

    // ---- wind the producer down and wait for it to release shared state ----
    ctx.stop.store(true, std::memory_order_release);
    esp_http_client_handle_t c = ctx.client.load(std::memory_order_acquire);
    if (c) esp_http_client_close(c);  // unblock a read stuck at network timeout
    for (int i = 0; i < 80 && !ctx.producer_done.load(std::memory_order_acquire);
         ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    std::free(drain);
    vSemaphoreDelete(ctx.data_new);
    vSemaphoreDelete(ctx.mutex);
    (void)producer;

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
        podcast_ui_set_playback(PodcastPlaybackState::Error,
                                (s_last_fail[0] != '\0')
                                    ? s_last_fail
                                    : "连接失败，正在重试");
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
