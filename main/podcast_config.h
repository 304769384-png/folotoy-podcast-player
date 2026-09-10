#pragma once

#include <cstddef>

// Built-in podcast subscriptions. v1 ships with a single curated source so the
// three-button device needs no on-screen URL entry. The list is the natural
// extension point for future multi-source support (settings page / BLE / web
// portal). Every Chinese character used in a display name MUST be included in
// the UI font symbol list consumed by tools/fonts/generate_fonts.sh.
struct PodcastSource {
    const char *name;      // shown on the main screen / future source list
    const char *feed_url;  // RSS 2.0 feed
};

constexpr PodcastSource kPodcastSources[] = {
    {"张小珺商业访谈录", "https://feed.xyzfm.space/dk4yh3pkpjp3"},
};

constexpr std::size_t kPodcastSourceCount =
    sizeof(kPodcastSources) / sizeof(kPodcastSources[0]);
