# -*- coding: utf-8 -*-
"""
流式 RSS 2.0 播客订阅解析器 —— 参考实现（黄金模型，字节驱动）

与固件 main/podcast_feed_parser.cc 同构：
- feed() 接受任意边界数据块（模拟 esp_http_client_read 的分片）；
- 不全量缓冲 XML；定长字段 title_cap/url_cap（单位：字节），最多 max_episodes 条；
- 只取 <item> 内 <title> 文本 与 <enclosure url="...">；
- 标题以裸 UTF-8 字节保存（与屏幕字库管线一致，解析器不做 UTF-8 解码）；
- 跳过注释/CDATA 外壳（CDATA 内标题字节照常采集）、识别自闭合标签；
- 基础 HTML 实体解码，非 ASCII 码点编码为 UTF-8 字节；
- 满额即 finished，调用方可立刻断开 HTTP。
"""

ENTITIES = {b'amp': 0x26, b'lt': 0x3C, b'gt': 0x3E,
            b'quot': 0x22, b'apos': 0x27, b'nbsp': 0x20}

TEXT, LT, BANG, NAME = 0, 1, 2, 3
ATTR_SKIP, ATTR_NAME, ATTR_WAIT, ATTR_EQ, ATTR_VAL, ATTR_RAW = 4, 5, 6, 7, 8, 9
CDATA, COMMENT, ENTITY = 10, 11, 12


def utf8(cp):
    if cp < 0x80:
        return bytes([cp])
    if cp < 0x800:
        return bytes([0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)])
    if cp < 0x10000:
        return bytes([0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F),
                      0x80 | (cp & 0x3F)])
    return bytes([0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
                  0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)])


def is_alpha(b):
    return 0x41 <= b <= 0x5A or 0x61 <= b <= 0x7A


def is_alnum(b):
    return is_alpha(b) or 0x30 <= b <= 0x39


def is_space(b):
    return b in (0x20, 0x09, 0x0D, 0x0A)


def utf8_trim(buf):
    """丢弃末尾被定长缓冲截断的半个 UTF-8 字符，防止屏幕花屏。"""
    i = len(buf)
    while i > 0 and 0x80 <= buf[i - 1] <= 0xBF:
        i -= 1
    if i == 0 or buf[i - 1] < 0x80:
        return bytes(buf)
    lead = buf[i - 1]
    need = (2 if 0xC0 <= lead < 0xE0 else
            3 if 0xE0 <= lead < 0xF0 else
            4 if 0xF0 <= lead < 0xF8 else 1)
    return bytes(buf[:i - 1]) if len(buf) - (i - 1) < need else bytes(buf)


class FeedParser:
    def __init__(self, max_episodes=8, title_cap=128, url_cap=256):
        self.max_episodes = max_episodes
        self.title_cap = title_cap
        self.url_cap = url_cap
        self.episodes = []
        self.finished = False
        self._reset()

    def _reset(self):
        self.state = TEXT
        self.stack = []
        self.tag = bytearray()
        self.closing = self.sc = False
        self.attr = bytearray()
        self.val = bytearray()
        self.quote = 0
        self.bang = bytearray()
        self.mk = 0
        self.entity = bytearray()
        self.capture = False
        self.title = bytearray()
        self.ep = None
        self.url_attr = b''

    def _put(self, b):
        if self.capture and len(self.title) < self.title_cap - 1:
            self.title.append(b)

    def _put_seq(self, bs):
        for b in bs:
            self._put(b)

    def _flush_entity(self):
        name = bytes(self.entity)
        if name[:1] == b'#':
            try:
                body = name[1:]
                cp = int(body, 16) if body[:1] in (b'x', b'X') else int(body)
                if 0 <= cp <= 0x10FFFF:
                    self._put_seq(utf8(cp))
            except ValueError:
                pass
        elif name in ENTITIES:
            self._put(ENTITIES[name])

    def _closing_seq(self, b, target):
        """逐字节匹配 target；失配时把已吞入的前缀字节退回输出通道。"""
        on_text = self._put if self.state == CDATA else (lambda x: None)
        if b == target[self.mk]:
            self.mk += 1
            if self.mk == len(target):
                self.state = TEXT
                self.mk = 0
        else:
            if self.mk > 0:
                on_text_seq = (self._put_seq if self.state == CDATA
                               else (lambda x: None))
                on_text_seq(target[:self.mk])
                self.mk = 0
            if b == target[0]:
                self.mk = 1
            else:
                on_text(b)

    def feed(self, data):
        if isinstance(data, str):
            data = data.encode('utf-8', 'ignore')
        for b in data:
            self._step(b)
            if self.finished:
                return

    def _step(self, b):
        s = self.state

        if s == TEXT:
            if b == 0x26:           # &
                self.entity = bytearray()
                self.state = ENTITY
            elif b == 0x3C:         # <
                self.tag = bytearray()
                self.closing = self.sc = False
                self.bang = bytearray()
                self.state = LT
            else:
                self._put(b)
            return

        if s == ENTITY:
            if b == 0x3B:           # ;
                self._flush_entity()
                self.state = TEXT
            else:
                self.entity.append(b)
                if len(self.entity) > 12:
                    self.state = TEXT
            return

        if s == LT:
            if b == 0x2F:
                self.closing = True
                self.state = NAME
            elif b == 0x21:
                self.state = BANG
            elif is_alpha(b):
                self.tag.append(b | 0x20)
                self.state = NAME
            elif b == 0x3E:
                self.state = TEXT
            return

        if s == BANG:
            self.bang.append(b)
            if bytes(self.bang) == b'[CDATA[':
                self.mk = 0
                self.state = CDATA
            elif bytes(self.bang) == b'--':
                self.mk = 0
                self.state = COMMENT
            elif b == 0x3E and not self.bang[:1] in (b'-', b'['):
                self.state = TEXT
            elif len(self.bang) > 8:
                self.state = COMMENT if self.bang[:1] == b'-' else TEXT
            return

        if s == CDATA:
            self._closing_seq(b, b']]>')
            return

        if s == COMMENT:
            self._closing_seq(b, b'-->')
            return

        if s == NAME:
            if is_alnum(b) or b in (0x2D, 0x5F, 0x3A):
                self.tag.append(b | 0x20 if is_alpha(b) else b)
            elif b == 0x2F:
                self.sc = True
            elif b == 0x3E:
                self._finish_tag()
            else:
                self.attr = bytearray()
                self.url_attr = b''
                self.state = ATTR_SKIP
            return

        if s == ATTR_SKIP:
            if b == 0x3E:
                self._finish_tag()
            elif b == 0x2F:
                self.sc = True
            elif is_alpha(b) or b == 0x5F:
                self.attr = bytearray([b | 0x20])
                self.state = ATTR_NAME
            return

        if s == ATTR_NAME:
            if is_alnum(b) or b in (0x2D, 0x5F, 0x3A):
                self.attr.append(b | 0x20 if is_alpha(b) else b)
            elif b == 0x3D:
                self.state = ATTR_EQ
            elif is_space(b):
                self.state = ATTR_WAIT
            elif b == 0x2F:
                self.sc = True
                self.state = ATTR_SKIP
            elif b == 0x3E:
                self._finish_tag()
            return

        if s == ATTR_WAIT:
            if b == 0x3D:
                self.state = ATTR_EQ
            elif b == 0x3E:
                self._finish_tag()
            elif b == 0x2F:
                self.sc = True
                self.state = ATTR_SKIP
            elif not is_space(b):
                self.attr = bytearray([b | 0x20 if is_alpha(b) else b])
                self.state = ATTR_NAME
            return

        if s == ATTR_EQ:
            if b in (0x22, 0x27):
                self.quote = b
                self.val = bytearray()
                self.state = ATTR_VAL
            elif not is_space(b):
                self.val = bytearray([b])
                self.state = ATTR_RAW
            return

        if s == ATTR_VAL:
            if b == self.quote:
                self._save_attr()
                self.state = ATTR_SKIP
            elif len(self.val) < self.url_cap - 1:
                self.val.append(b)
            return

        if s == ATTR_RAW:
            if is_space(b) or b == 0x3E:
                self._save_attr()
                if b == 0x3E:
                    self._finish_tag()
                else:
                    self.state = ATTR_SKIP
            elif len(self.val) < self.url_cap - 1:
                self.val.append(b)
            return

    def _save_attr(self):
        if bytes(self.tag) == b'enclosure' and bytes(self.attr) == b'url':
            self.url_attr = bytes(self.val)

    def _finish_tag(self):
        name = bytes(self.tag)
        closing, sc = self.closing, self.sc
        self.state = TEXT

        if name == b'item':
            if closing:
                if self.ep is not None:
                    self.ep['title'] = utf8_trim(self.title).strip()
                    if self.ep['url']:
                        self.episodes.append(self.ep)
                self.ep = None
                self.title = bytearray()
                if self.stack and self.stack[-1] == b'item':
                    self.stack.pop()
                if len(self.episodes) >= self.max_episodes:
                    self.finished = True
            elif not sc:
                self.ep = {'title': b'', 'url': b''}
                self.title = bytearray()
                self.stack.append(b'item')
            return

        if name == b'enclosure' and not closing and self.ep is not None and self.url_attr:
            self.ep['url'] = self.url_attr[: self.url_cap - 1]
            return

        if name == b'title':
            if closing:
                self.capture = False
                if self.stack and self.stack[-1] == b'title':
                    self.stack.pop()
            elif not sc and self.stack and self.stack[-1] == b'item':
                self.title = bytearray()
                self.capture = True
                self.stack.append(b'title')
