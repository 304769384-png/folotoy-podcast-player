#include "podcast_feed_parser.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr char kEntityAmp[] = "amp";
constexpr char kEntityLt[] = "lt";
constexpr char kEntityGt[] = "gt";
constexpr char kEntityQuot[] = "quot";
constexpr char kEntityApos[] = "apos";
constexpr char kEntityNbsp[] = "nbsp";

bool is_alpha(uint8_t b) {
    return (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z');
}

bool is_digit(uint8_t b) { return b >= '0' && b <= '9'; }

bool is_alnum(uint8_t b) { return is_alpha(b) || is_digit(b); }

bool is_space(uint8_t b) {
    return b == ' ' || b == '\t' || b == '\r' || b == '\n';
}

uint8_t lower_ascii(uint8_t b) { return is_alpha(b) ? static_cast<uint8_t>(b | 0x20) : b; }

// Encodes a Unicode code point as UTF-8 into out. Returns bytes written.
std::size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

// Drops a trailing half UTF-8 character cut by the fixed title buffer.
std::size_t utf8_trim_len(const char *buf, std::size_t len) {
    std::size_t i = len;
    while (i > 0 && static_cast<uint8_t>(buf[i - 1]) >= 0x80 &&
           static_cast<uint8_t>(buf[i - 1]) <= 0xBF) {
        --i;
    }
    if (i == 0 || static_cast<uint8_t>(buf[i - 1]) < 0x80) return len;
    const uint8_t lead = static_cast<uint8_t>(buf[i - 1]);
    std::size_t need = 1;
    if (lead >= 0xC0 && lead < 0xE0) need = 2;
    else if (lead >= 0xE0 && lead < 0xF0) need = 3;
    else if (lead >= 0xF0 && lead < 0xF8) need = 4;
    return (len - (i - 1) < need) ? i - 1 : len;
}

bool stack_top_is(PodcastFeedParser *p, const char *name) {
    return p->stack_depth > 0 && std::strcmp(p->stack[p->stack_depth - 1], name) == 0;
}

void stack_push(PodcastFeedParser *p, const char *name) {
    if (p->stack_depth < sizeof(p->stack) / sizeof(p->stack[0])) {
        std::snprintf(p->stack[p->stack_depth++], sizeof(p->stack[0]), "%s", name);
    }
}

void stack_pop(PodcastFeedParser *p) {
    if (p->stack_depth > 0) --p->stack_depth;
}

void put_title(PodcastFeedParser *p, uint8_t b) {
    if (p->capture && p->title_len + 1 < sizeof(p->title)) {
        p->title[p->title_len++] = static_cast<char>(b);
    }
}

void flush_entity(PodcastFeedParser *p) {
    const char *name = p->entity;
    if (name[0] == '#') {
        uint32_t cp = 0;
        bool ok = true;
        const char *body = name + 1;
        if ((body[0] == 'x' || body[0] == 'X') && body[1]) {
            for (const char *q = body + 1; *q; ++q) {
                uint8_t d = static_cast<uint8_t>(*q);
                uint32_t v = is_digit(d) ? d - '0' :
                             (d >= 'a' && d <= 'f') ? d - 'a' + 10 :
                             (d >= 'A' && d <= 'F') ? d - 'A' + 10 : 0xFF;
                if (v == 0xFF) { ok = false; break; }
                cp = cp * 16 + v;
            }
        } else if (body[0]) {
            for (const char *q = body; *q; ++q) {
                if (!is_digit(static_cast<uint8_t>(*q))) { ok = false; break; }
                cp = cp * 10 + static_cast<uint32_t>(*q - '0');
            }
        } else {
            ok = false;
        }
        if (ok && cp <= 0x10FFFF && p->title_len + 4 < sizeof(p->title)) {
            p->title_len += utf8_encode(cp, p->title + p->title_len);
        }
        return;
    }
    uint8_t ch = 0;
    if (std::strcmp(name, kEntityAmp) == 0) ch = '&';
    else if (std::strcmp(name, kEntityLt) == 0) ch = '<';
    else if (std::strcmp(name, kEntityGt) == 0) ch = '>';
    else if (std::strcmp(name, kEntityQuot) == 0) ch = '"';
    else if (std::strcmp(name, kEntityApos) == 0) ch = '\'';
    else if (std::strcmp(name, kEntityNbsp) == 0) ch = ' ';
    if (ch) put_title(p, ch);
}

// Byte-wise match of a closing sequence (]]> / -->). When the match fails, the
// swallowed prefix is replayed to the text sink (CDATA only; comments drop it).
void closing_seq(PodcastFeedParser *p, uint8_t b, const char *target) {
    const bool emit_text = p->state == PodcastFeedParser::CDATA;
    if (b == static_cast<uint8_t>(target[p->match_k])) {
        ++p->match_k;
        if (p->match_k == std::strlen(target)) {
            p->state = PodcastFeedParser::TEXT;
            p->match_k = 0;
        }
        return;
    }
    if (p->match_k > 0) {
        if (emit_text) {
            for (std::size_t i = 0; i < p->match_k; ++i) {
                put_title(p, static_cast<uint8_t>(target[i]));
            }
        }
        p->match_k = 0;
    }
    if (b == static_cast<uint8_t>(target[0])) p->match_k = 1;
    else if (emit_text) put_title(p, b);
}

void append(char *buf, std::size_t cap, std::size_t *len, char ch) {
    if (*len + 1 < cap) buf[(*len)++] = ch;
}

void emit_episode(PodcastFeedParser *p) {
    // Trim ASCII whitespace and any half UTF-8 character at the title tail.
    std::size_t len = p->title_len;
    while (len > 0 && is_space(static_cast<uint8_t>(p->title[len - 1]))) --len;
    std::size_t head = 0;
    while (head < len && is_space(static_cast<uint8_t>(p->title[head]))) ++head;
    len = utf8_trim_len(p->title, len);
    if (len <= head || p->pending_url[0] == '\0' || p->count >= p->capacity) {
        return;
    }
    PodcastEpisode *out = &p->episodes[p->count];
    const std::size_t copy = len - head < sizeof(out->title) - 1
                                 ? len - head : sizeof(out->title) - 1;
    std::memcpy(out->title, p->title + head, copy);
    out->title[copy] = '\0';
    std::snprintf(out->url, sizeof(out->url), "%s", p->pending_url);
    ++p->count;
}

void save_attr(PodcastFeedParser *p) {
    if (std::strcmp(p->tag, "enclosure") == 0 &&
        std::strcmp(p->attr, "url") == 0) {
        std::snprintf(p->pending_url, sizeof(p->pending_url), "%s", p->val);
    }
}

void finish_tag(PodcastFeedParser *p) {
    const char *name = p->tag;
    p->state = PodcastFeedParser::TEXT;

    if (std::strcmp(name, "item") == 0) {
        if (p->closing) {
            if (p->has_episode) emit_episode(p);
            p->has_episode = false;
            p->title_len = 0;
            p->pending_url[0] = '\0';
            if (stack_top_is(p, "item")) stack_pop(p);
            if (p->count >= p->capacity) p->finished = true;
        } else if (!p->self_closing && p->count < p->capacity) {
            p->has_episode = true;
            p->title_len = 0;
            p->pending_url[0] = '\0';
            stack_push(p, "item");
        }
        return;
    }

    // enclosure is self closing; its url was captured by save_attr() and is
    // consumed together with the title when the parent </item> arrives.
    if (std::strcmp(name, "enclosure") == 0) return;

    if (std::strcmp(name, "title") == 0) {
        if (p->closing) {
            p->capture = false;
            if (stack_top_is(p, "title")) stack_pop(p);
        } else if (!p->self_closing && stack_top_is(p, "item")) {
            p->title_len = 0;
            p->capture = true;
            stack_push(p, "title");
        }
    }
}

void step(PodcastFeedParser *p, uint8_t b) {
    switch (p->state) {
        case PodcastFeedParser::TEXT:
            if (b == '&') {
                p->entity_len = 0;
                p->entity[0] = '\0';
                p->state = PodcastFeedParser::ENTITY;
            } else if (b == '<') {
                p->tag[0] = '\0';
                p->closing = p->self_closing = false;
                p->bang_len = 0;
                p->state = PodcastFeedParser::LT;
            } else {
                put_title(p, b);
            }
            return;

        case PodcastFeedParser::ENTITY:
            if (b == ';') {
                p->entity[p->entity_len] = '\0';
                flush_entity(p);
                p->state = PodcastFeedParser::TEXT;
            } else {
                append(p->entity, sizeof(p->entity), &p->entity_len,
                       static_cast<char>(b));
                if (p->entity_len + 1 >= sizeof(p->entity)) p->state = PodcastFeedParser::TEXT;
            }
            return;

        case PodcastFeedParser::LT:
            if (b == '/') {
                p->closing = true;
                p->state = PodcastFeedParser::NAME;
            } else if (b == '!') {
                p->bang_len = 0;
                p->state = PodcastFeedParser::BANG;
            } else if (is_alpha(b)) {
                // Seed the tag name; further bytes accumulate in NAME using
                // strlen as the length tracker.
                p->tag[0] = static_cast<char>(lower_ascii(b));
                p->tag[1] = '\0';
                p->state = PodcastFeedParser::NAME;
            } else if (b == '>') {
                p->state = PodcastFeedParser::TEXT;
            }
            return;

        case PodcastFeedParser::BANG:
            if (p->bang_len < sizeof(p->bang)) {
                p->bang[p->bang_len++] = static_cast<char>(b);
                p->bang[p->bang_len] = '\0';
            }
            if (std::strcmp(p->bang, "[CDATA[") == 0) {
                p->match_k = 0;
                p->state = PodcastFeedParser::CDATA;
            } else if (std::strcmp(p->bang, "--") == 0) {
                p->match_k = 0;
                p->state = PodcastFeedParser::COMMENT;
            } else if (b == '>' && p->bang[0] != '-' && p->bang[0] != '[') {
                p->state = PodcastFeedParser::TEXT;
            } else if (p->bang_len >= sizeof(p->bang)) {
                p->state = (p->bang[0] == '-') ? PodcastFeedParser::COMMENT
                                               : PodcastFeedParser::TEXT;
            }
            return;

        case PodcastFeedParser::CDATA:
            closing_seq(p, b, "]]>");
            return;

        case PodcastFeedParser::COMMENT:
            closing_seq(p, b, "-->");
            return;

        case PodcastFeedParser::NAME:
            if (is_alnum(b) || b == '-' || b == '_' || b == ':') {
                std::size_t len = std::strlen(p->tag);
                append(p->tag, sizeof(p->tag), &len, static_cast<char>(lower_ascii(b)));
            } else if (b == '/') {
                p->self_closing = true;
            } else if (b == '>') {
                finish_tag(p);
            } else {
                p->attr[0] = '\0';
                // pending_url survives between attributes of one tag
                p->state = PodcastFeedParser::ATTR_SKIP;
            }
            return;

        case PodcastFeedParser::ATTR_SKIP:
            if (b == '>') finish_tag(p);
            else if (b == '/') p->self_closing = true;
            else if (is_alpha(b) || b == '_') {
                p->attr[0] = static_cast<char>(lower_ascii(b));
                p->attr[1] = '\0';
                p->state = PodcastFeedParser::ATTR_NAME;
            }
            return;

        case PodcastFeedParser::ATTR_NAME:
            if (is_alnum(b) || b == '-' || b == '_' || b == ':') {
                std::size_t len = std::strlen(p->attr);
                append(p->attr, sizeof(p->attr), &len,
                       static_cast<char>(lower_ascii(b)));
            } else if (b == '=') {
                p->state = PodcastFeedParser::ATTR_EQ;
            } else if (is_space(b)) {
                p->state = PodcastFeedParser::ATTR_WAIT;
            } else if (b == '/') {
                p->self_closing = true;
                p->state = PodcastFeedParser::ATTR_SKIP;
            } else if (b == '>') {
                finish_tag(p);
            }
            return;

        case PodcastFeedParser::ATTR_WAIT:
            if (b == '=') p->state = PodcastFeedParser::ATTR_EQ;
            else if (b == '>') finish_tag(p);
            else if (b == '/') {
                p->self_closing = true;
                p->state = PodcastFeedParser::ATTR_SKIP;
            } else if (!is_space(b)) {
                p->attr[0] = static_cast<char>(lower_ascii(b));
                p->attr[1] = '\0';
                p->state = PodcastFeedParser::ATTR_NAME;
            }
            return;

        case PodcastFeedParser::ATTR_EQ:
            if (b == '"' || b == '\'') {
                p->quote = static_cast<char>(b);
                p->val[0] = '\0';
                p->state = PodcastFeedParser::ATTR_VAL;
            } else if (!is_space(b)) {
                p->val[0] = static_cast<char>(b);
                p->val[1] = '\0';
                p->state = PodcastFeedParser::ATTR_RAW;
            }
            return;

        case PodcastFeedParser::ATTR_VAL: {
            if (b == static_cast<uint8_t>(p->quote)) {
                save_attr(p);
                p->state = PodcastFeedParser::ATTR_SKIP;
            } else {
                std::size_t len = std::strlen(p->val);
                append(p->val, sizeof(p->val), &len, static_cast<char>(b));
            }
            return;
        }

        case PodcastFeedParser::ATTR_RAW: {
            if (is_space(b) || b == '>') {
                save_attr(p);
                if (b == '>') finish_tag(p);
                else p->state = PodcastFeedParser::ATTR_SKIP;
            } else {
                std::size_t len = std::strlen(p->val);
                append(p->val, sizeof(p->val), &len, static_cast<char>(b));
            }
            return;
        }
    }
}

}  // namespace

void podcast_feed_parser_init(PodcastFeedParser *parser,
                              PodcastEpisode *output, std::size_t capacity) {
    if (!parser || !output || capacity == 0) return;
    std::memset(parser, 0, sizeof(*parser));
    parser->episodes = output;
    parser->capacity = capacity;
    parser->state = PodcastFeedParser::TEXT;
}

void podcast_feed_parser_feed(PodcastFeedParser *parser,
                              const uint8_t *data, std::size_t len) {
    if (!parser || !data || parser->finished) return;
    for (std::size_t i = 0; i < len && !parser->finished; ++i) {
        step(parser, data[i]);
    }
}
