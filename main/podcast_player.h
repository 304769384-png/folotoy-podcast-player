#pragma once

#include "podcast_feed_parser.h"

#include <cstddef>
#include <cstdint>

enum class PodcastPlaybackState {
    Stopped,
    Connecting,
    Buffering,
    Playing,
    Reconnecting,
    Error,
};

bool podcast_player_init();
void podcast_player_set_network(bool connected);
void podcast_player_toggle();
void podcast_player_set_playing(bool playing);
bool podcast_player_is_playing();

void podcast_player_select_relative(int delta);
void podcast_player_set_episode(std::size_t index);
std::size_t podcast_player_index();
std::size_t podcast_player_count();
PodcastEpisode podcast_player_episode(std::size_t index);

// Replaces the whole catalog. The listener resumes on the remembered episode
// title when the refreshed feed still contains it.
bool podcast_player_replace_episodes(const PodcastEpisode *episodes,
                                     std::size_t count);

void podcast_player_set_volume(uint8_t volume);
uint8_t podcast_player_volume();
