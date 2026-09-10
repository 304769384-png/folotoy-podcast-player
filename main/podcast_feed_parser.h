#pragma once

#include <cstddef>
#include <cstdint>

// One podcast episode extracted from an RSS <item>. Fields are fixed size and
// hold raw UTF-8 bytes (the screen font pipeline consumes UTF-8 directly).
struct PodcastEpisode {
    char title[128];
    char url[256];
};

// Streaming RSS 2.0 parser. Feed it arbitrary byte chunks from the network;
// it never buffers the whole document. State is kept in this struct so the
// parser has no heap dependency and can be unit tested on a host build.
//
// The Python reference implementation lives in
// tools/feed_prototype/feed_parser.py and is the golden model; keep the two in
// sync. It extracts, per item, the <title> text and the <enclosure url="...">,
// skipping comments/CDATA wrappers and decoding basic HTML entities.
struct PodcastFeedParser {
    PodcastEpisode *episodes;
    std::size_t capacity;
    std::size_t count;
    bool finished;

    // --- scanner state (mirrors the Python golden model) ---
    enum State {
        TEXT = 0, LT, BANG, NAME,
        ATTR_SKIP, ATTR_NAME, ATTR_WAIT, ATTR_EQ, ATTR_VAL, ATTR_RAW,
        CDATA, COMMENT, ENTITY,
    };
    State state;
    char stack[8][12];       // open element names (lowercase)
    std::size_t stack_depth;
    char tag[16];
    bool closing;
    bool self_closing;
    char attr[16];
    char val[256];
    char quote;
    char bang[9];
    std::size_t bang_len;
    std::size_t match_k;     // partial match length of ]]> / -->
    char entity[14];
    std::size_t entity_len;
    bool capture;
    char title[128];
    std::size_t title_len;
    bool has_episode;
    char pending_url[256];
};

void podcast_feed_parser_init(PodcastFeedParser *parser,
                              PodcastEpisode *output, std::size_t capacity);

// Process a chunk. Safe to call again after finished == true (it is a no-op).
void podcast_feed_parser_feed(PodcastFeedParser *parser,
                              const uint8_t *data, std::size_t len);
