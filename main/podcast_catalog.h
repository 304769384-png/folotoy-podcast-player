#pragma once

#include "podcast_feed_parser.h"

#include <cstddef>

// Streams a podcast RSS feed over HTTP(S) and extracts the newest episodes
// directly from the wire chunks. The feed is never buffered as a whole (real
// feeds can be 1.5 MB while the ESP32-C3 has ~400 KB SRAM).
//
// Returns the number of episodes written (newest first), 0 on failure.
std::size_t podcast_catalog_fetch(const char *feed_url,
                                  PodcastEpisode *episodes,
                                  std::size_t capacity);
