# Podcast Player for FoloToy AI Passport

把 FoloToy AI Passport（ESP32-C3 卡片设备）变成一台随身播客机。开机自动拉取订阅源，
三键选期、播放、定时关闭；v0.1 内置订阅 **《张小珺Jùn｜商业访谈录》**。

## 功能

- 联网后自动获取 RSS，提取最新 8 期，标题在 240×320 屏幕上多行显示
- 同时支持 **M4A/AAC**（小宇宙等主流托管）与 MP3 节目，按地址自动选择解码器
- 一期播完自动播放下一期；记忆上次选择的期数
- 设备端屏幕配 Wi-Fi（三键键盘）+ 网页配网门户（热点 `Podcast` / `192.168.4.1`）
- 音量 / 定时播放（15/30/60/90 分钟到点深睡）/ 屏幕亮度与自动熄屏 / 网络管理
- 中文字体在 CI 构建时生成：UI 字库（4bpp）+ GB2312 全集 fallback（1bpp），
  任意节目标题汉字均可显示
- 不含任何 Wi-Fi 凭据，分享安全

## 按键

| 操作 | 功能 |
|---|---|
| 上 / 下（短按） | 切换节目 |
| 确定（短按） | 播放 / 暂停 |
| 确定（长按 0.85s） | 进入 / 退出设置；设置内逐级返回 |

## 刷写固件（无需安装开发环境）

1. 用 **Chrome 或 Edge** 打开 <https://tool.folotoy.com/index>
2. 设备开机状态下用数据线连接电脑（Windows 识别不到设备时先装串口驱动，
   波特率选 921600）
3. 在本仓库 GitHub **Actions** 页面下载最新成功构建的 artifact
   `Podcast-Player-firmware`，解压得到：
   - `Podcast-Player-full.bin` —— **首次安装**：浏览器中地址填 `0x0`
   - `build/Podcast-Player.bin`（app）—— 后续升级：地址填 `0x10000`，
     保留 Wi-Fi 配置与设备身份
4. 写入完成后设备自动重启，首次开机进入配网

> 安全红线：不要使用网页工具的"抹除"功能。本固件沿用原分区表，
> `cardid`（设备身份）与 `recovery`（救砖）分区保持不动。

## 更换或增加订阅源

编辑 `main/podcast_config.h`：

```cpp
constexpr PodcastSource kPodcastSources[] = {
    {"显示名称", "https://example.com/feed.xml"},
};
```

要求：标准 RSS 2.0；音频为 MP3 或 M4A(AAC)；音频地址支持 HTTP 顺序流式播放
（M4A 的 `moov` 索引需前置，小宇宙/多数播客托管均满足）。
提交后 CI 自动产出新固件。多源切换界面与 BLE 手机订阅管理在路线图上。

## 工作原理（与网络收音机的关键差异）

播客 RSS 常达 1 MB 以上，而 ESP32-C3 仅约 400 KB SRAM，因此节目单解析器
（`main/podcast_feed_parser.cc`）是**流式状态机**：网络块到达即逐字节解析，
只保留每期定长标题与地址，集满 8 期立即停止下载，从不缓冲整篇 XML。
其 Python 黄金模型在 `tools/feed_prototype/`，两端以同一真实 feed 对拍。

## 本地 / 云端构建

- **云端（推荐零基础）**：推送到 GitHub，`.github/workflows/build.yml`
  自动完成 host 测试、字体生成、`idf.py build` 与 full 镜像合并，产物在 artifact。
- **本地**：ESP-IDF v5.5.x，

  ```bash
  export FEED_XML=/tmp/feed.xml
  curl -A test -o "$FEED_XML" https://feed.xyzfm.space/dk4yh3pkpjp3
  bash tools/tests/run_tests.sh        # 解析器 host 测试
  bash tools/fonts/generate_fonts.sh   # 生成 main/pod_font*.c
  idf.py set-target esp32c3
  idf.py build
  ```

## 已知限制 / 路线图

- M4A 顺序流式播放已在官方解码器支持范围内，但无 PSRAM 的 C3 对超大
  `moov` 表的实际内存表现以实机验证为准
- 暂不记忆一期内播放进度（暂停后续播需从头开始）
- 单订阅源；三键不支持输入任意 RSS 地址（计划用 BLE 手机端解决）
- 无经典蓝牙，不能连接蓝牙耳机（芯片仅有 BLE）

## 致谢

基于 leo-radio（LEO RADIO 城市网络收音机）的板级支持包、配网门户与 UI 框架改造，
音频解码使用 Espressif esp_audio_codec，字体为 Noto Sans SC（SIL OFL）。
MIT License。
