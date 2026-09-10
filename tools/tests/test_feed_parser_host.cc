// Host unit test for the streaming RSS parser. No ESP-IDF dependency.
// Build: g++ -std=c++17 -I main main/podcast_feed_parser.cc \
//             tools/tests/test_feed_parser_host.cc -o test_feed_parser
// Run:   ./test_feed_parser [real_feed.xml]
#include "podcast_feed_parser.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char *name) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) ++g_failures;
}

std::vector<PodcastEpisode> parse_chunks(const std::string &data,
                                         std::size_t chunk,
                                         std::size_t cap = 8) {
    std::vector<PodcastEpisode> out(cap);
    PodcastFeedParser parser;
    podcast_feed_parser_init(&parser, out.data(), cap);
    for (std::size_t i = 0; i < data.size(); i += chunk) {
        podcast_feed_parser_feed(
            &parser,
            reinterpret_cast<const uint8_t *>(data.data() + i),
            std::min<std::size_t>(chunk, data.size() - i));
        if (parser.finished) break;
    }
    out.resize(parser.count);
    return out;
}

bool same(const std::vector<PodcastEpisode> &a,
          const std::vector<PodcastEpisode> &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::strcmp(a[i].title, b[i].title) != 0 ||
            std::strcmp(a[i].url, b[i].url) != 0)
            return false;
    }
    return true;
}

void synthetic_tests() {
    const std::string xml =
        "<?xml version=\"1.0\"?><rss><channel>"
        "<title>\xe6\x92\xad\xe5\xae\xa2\xe6\x80\xbb\xe5\x90\x8d</title>"
        "<!-- <item><title>fake</title></item> -->"
        "<item><title>\xe7\xac\xac1\xe6\x9c\x9f\xef\xbc\x9aA &amp; B &#65; "
        "&quot;\xe5\xbc\x95\xe5\x8f\xb7&quot;</title>"
        "<enclosure length=\"123\" url=\"https://a.test/1.m4a?x=1\" "
        "type=\"audio/mp4\"/></item>"
        "<item><enclosure url='https://b.test/2.mp3' type='audio/mpeg' />"
        "<title>  trim me  </title></item>"
        "<item><title>no url dropped</title></item>"
        "<item><title><![CDATA[cdata &amp; kept]]></title>"
        "<enclosure url=\"https://d.test/4.m4a\"/></item>"
        "</channel></rss>";

    auto eps = parse_chunks(xml, 1);  // 1-byte chunks force every boundary
    check(eps.size() == 3, "host synthetic count");
    check(eps.size() == 3 &&
          std::string(eps[0].title) ==
              "\xe7\xac\xac1\xe6\x9c\x9f\xef\xbc\x9aA & B A "
              "\"\xe5\xbc\x95\xe5\x8f\xb7\"",
          "host synthetic title 1");
    check(eps.size() == 3 && std::string(eps[1].title) == "trim me",
          "host synthetic title 2 trimmed");
    check(eps.size() == 3 && std::string(eps[2].title) == "cdata &amp; kept",
          "host cdata entity kept literal");
    check(eps.size() == 3 && std::string(eps[0].url) == "https://a.test/1.m4a?x=1" &&
          std::string(eps[1].url) == "https://b.test/2.mp3" &&
          std::string(eps[2].url) == "https://d.test/4.m4a",
          "host synthetic urls");
    check(parse_chunks(xml, 3, 2).size() == 2, "host max episodes");
}

void real_feed_tests(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("SKIP real feed (%s)\n", path.c_str());
        return;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    const std::string data = ss.str();

    const std::size_t sizes[] = {1, 3, 7, 64, 256, 1024, 4096, 16384,
                                 std::string::npos};
    std::vector<PodcastEpisode> ref;
    bool first = true;
    for (std::size_t sz : sizes) {
        auto eps = parse_chunks(data, sz == std::string::npos ? data.size() : sz);
        if (first) {
            ref = eps;
            first = false;
        }
        char name[40];
        std::snprintf(name, sizeof(name), "host chunk=%zu identical",
                      sz == std::string::npos ? data.size() : sz);
        check(same(eps, ref), name);
    }
    std::printf("\n%s: %zu bytes, %zu episodes\n", path.c_str(), data.size(),
                ref.size());
    for (std::size_t i = 0; i < ref.size(); ++i) {
        std::printf("[%zu] %s\n    %s\n", i, ref[i].title, ref[i].url);
    }
}

}  // namespace

int main(int argc, char **argv) {
    synthetic_tests();
    if (argc > 1) real_feed_tests(argv[1]);
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL HOST TESTS PASSED\n",
                g_failures);
    return g_failures ? 1 : 0;
}
