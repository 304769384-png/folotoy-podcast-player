# -*- coding: utf-8 -*-
"""
J2 验证：
1. 合成 XML 边界用例（CDATA 标题/实体/注释/channel-title 区分/属性乱序/单引号）；
2. 真实张小珺 feed 以多种 chunk 粒度喂入，结果必须逐字节一致（chunk 边界无关性）。
用法: python test_parse.py [feed.xml路径]
"""
import os
import sys

from feed_parser import FeedParser


def parse_chunks(data, chunk_size, **kw):
    p = FeedParser(**kw)
    for i in range(0, len(data), chunk_size):
        p.feed(data[i:i + chunk_size])
        if p.finished:
            break
    return p.episodes


def dec(eps):
    return [(e['title'].decode('utf-8'), e['url'].decode('utf-8')) for e in eps]


def case(name, got, want):
    ok = got == want
    print(('PASS' if ok else 'FAIL'), name)
    if not ok:
        print('  got :', got)
        print('  want:', want)
        raise SystemExit(1)


def synthetic_tests():
    xml = (
        '<?xml version="1.0"?><rss><channel>'
        '<title>播客总名（绝不能被采集）</title>'
        '<!-- 注释里的 <item><title>假标题</title></item> -->'
        '<item>'
        '<title>第1期：A &amp; B &#65; &quot;引号&quot;</title>'
        '<enclosure length="123" url="https://a.test/1.m4a?x=1" type="audio/mp4"/>'
        '</item>'
        '<item>'
        '<enclosure url=\'https://b.test/2.mp3\' type="audio/mpeg" />'
        '<title>  第 2 期 空格修剪  </title>'
        '</item>'
        '<item><title>无地址的一期应被丢弃</title></item>'
        '<item><title><![CDATA[CDATA内 &amp; 原样保留]]></title>'
        '<enclosure url="https://d.test/4.m4a"/></item>'
        '</channel></rss>'
    ).encode('utf-8')

    eps = parse_chunks(xml, 1, max_episodes=8)  # 1 字节粒度强制跨块
    pairs = dec(eps)
    case('synthetic count', len(pairs), 3)
    case('synthetic titles', [t for t, _ in pairs],
         ['第1期：A & B A "引号"', '第 2 期 空格修剪',
          'CDATA内 &amp; 原样保留'])
    case('synthetic urls', [u for _, u in pairs],
         ['https://a.test/1.m4a?x=1', 'https://b.test/2.mp3',
          'https://d.test/4.m4a'])

    eps2 = parse_chunks(xml, 3, max_episodes=2)
    case('max_episodes truncation', len(eps2), 2)
    print('PASS synthetic suite')


def real_feed_tests(path):
    data = open(path, 'rb').read()
    sizes = [1, 3, 7, 64, 256, 1024, 4096, 16384, len(data)]
    ref = None
    for sz in sizes:
        eps = parse_chunks(data, sz, max_episodes=8, title_cap=128, url_cap=256)
        if ref is None:
            ref = eps
        case(f'chunk={sz} identical', eps, ref)
    pairs = dec(ref)
    print(f'\n真实 feed: {len(data)} 字节，解析出 {len(pairs)} 期（最新在前）\n')
    for i, (t, u) in enumerate(pairs):
        print(f"[{i}] {t}\n    {u}")
    assert all(u.startswith('http') for _, u in pairs)
    assert all(t for t, _ in pairs)
    print('\nPASS real-feed suite')


if __name__ == '__main__':
    synthetic_tests()
    feed_path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.environ.get('TEMP', '/tmp'), 'zxj_feed.xml')
    if os.path.exists(feed_path):
        real_feed_tests(feed_path)
    else:
        print(f'SKIP real feed（未找到 {feed_path}）')
