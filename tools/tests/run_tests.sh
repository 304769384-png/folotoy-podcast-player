#!/usr/bin/env bash
# Host-side tests (no ESP-IDF needed):
#  1. Python golden-model parser tests
#  2. C++ parser built with g++, cross-checked against the real feed
#
# FEED_XML=/path/to/feed.xml bash tools/tests/run_tests.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

echo "=== Python golden model ==="
python3 tools/feed_prototype/test_parse.py "${FEED_XML:-}"

echo "=== C++ host parser ==="
g++ -std=c++17 -Wall -Wextra -I main \
    main/podcast_feed_parser.cc \
    tools/tests/test_feed_parser_host.cc \
    -o /tmp/test_feed_parser
/tmp/test_feed_parser "${FEED_XML:-}"

echo "All host tests passed."
