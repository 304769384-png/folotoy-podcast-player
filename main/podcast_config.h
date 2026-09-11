#pragma once

#include <cstddef>

// Built-in podcast subscriptions. The three-button device has no on-screen URL
// entry, so sources are compiled in and cycled by long-pressing UP/DOWN. The
// list is the natural extension point for more sources (settings page / BLE /
// web portal).
//
// Every Chinese character used in a display name MUST be included in the UI
// font symbol list. That list is derived automatically from main/*.c(c,h) by
// tools/fonts/make_font_symbols.py, so adding a name here extends the font too,
// as long as the name itself lives in this header.
struct PodcastSource {
    const char *name;      // shown on the main screen / source list
    const char *feed_url;  // RSS 2.0 feed
};

constexpr PodcastSource kPodcastSources[] = {
    {"张小珺商业访谈录", "https://feed.xyzfm.space/dk4yh3pkpjp3"},
    {"硅谷101", "https://feeds.fireside.fm/sv101/rss"},
    {"晚点聊", "https://feeds.fireside.fm/latetalk/rss"},
    {"半拿铁", "https://proxy.wavpub.com/caffebreve.xml"},
    {"组织进化论", "https://feed.xyzfm.space/wb9n7uukpubp"},
};

constexpr std::size_t kPodcastSourceCount =
    sizeof(kPodcastSources) / sizeof(kPodcastSources[0]);
