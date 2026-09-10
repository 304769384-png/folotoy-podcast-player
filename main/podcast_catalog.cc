#include "podcast_catalog.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr char kTag[] = "podcast_catalog";
constexpr std::size_t kChunkSize = 1024;

bool starts_with(const char *text, const char *prefix) {
    return text && prefix && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

}  // namespace

std::size_t podcast_catalog_fetch(const char *feed_url,
                                  PodcastEpisode *episodes,
                                  std::size_t capacity) {
    if (!feed_url || !episodes || capacity == 0) return 0;

    esp_http_client_config_t config = {};
    config.url = feed_url;
    config.timeout_ms = 8000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 512;
    config.user_agent = "AI-Passport-Podcast/0.1";
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 4;
    if (starts_with(feed_url, "https://")) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "HTTP client init failed");
        return 0;
    }
    esp_http_client_set_header(client, "Accept", "application/rss+xml,application/xml,*/*");
    // We have no gzip decoder: ask for the plain document explicitly.
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    PodcastFeedParser parser;
    podcast_feed_parser_init(&parser, episodes, capacity);

    auto *chunk = static_cast<uint8_t *>(std::malloc(kChunkSize));
    if (!chunk) {
        esp_http_client_cleanup(client);
        return 0;
    }

    std::size_t result = 0;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(kTag, "Open failed: %s", feed_url);
            break;
        }
        const int content_length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(kTag, "GET %s returned HTTP %d", feed_url, status);
            break;
        }
        ESP_LOGI(kTag, "Fetching feed (HTTP %d, %d bytes declared)", status,
                 content_length);

        int empty_reads = 0;
        while (!parser.finished) {
            const int received =
                esp_http_client_read(client, reinterpret_cast<char *>(chunk),
                                     static_cast<int>(kChunkSize));
            if (received < 0) {
                ESP_LOGW(kTag, "Feed read error");
                break;
            }
            if (received == 0) {
                if (++empty_reads > 3) break;  // clean EOF
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            empty_reads = 0;
            podcast_feed_parser_feed(&parser, chunk, received);
        }
        result = parser.count;
    } while (false);

    std::free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(kTag, "Extracted %u episodes", static_cast<unsigned>(result));
    return result;
}
