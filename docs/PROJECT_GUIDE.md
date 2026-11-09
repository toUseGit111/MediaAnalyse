# MediaAnalyse 工程说明：目录 · 构建 · 打包 · 使用

> 适用版本：v1.0（Qt 6.10.3 + FFmpeg 9.0.2）
> 用途：内部媒体排障工具（本地文件 / RTSP / RTMP / HLS / HTTP-FLV 的可视化解析）
> 配套文档：[`README.md`](../README.md)（设计与行为细节）、[`RELEASE.md`](../RELEASE.md)（发布验收）、[`REFACTOR_PLAN.md`](../REFACTOR_PLAN.md)（阶段计划）

---

## 1. 一分钟上手

```bash
# 1) 构建（在仓库根目录执行）
cmake --preset win-mingw-app
cmake --build --preset win-mingw-app --parallel 8

# 2) 运行 GUI
build/win-mingw-app/media-analyse.exe

# 3) 或直接用命令行分析一个文件
build/win-mingw-app/media-analyse-cli.exe --json tests/assets/sample.mp4

# 4) 打一个可分发的绿色包
python scripts/package.py --with-cli
```

---

## 2. 目录结构

仓库根目录（`D:\code\MediaAnalyse`）**就是** CMake 工程根，不再有嵌套的二级工程目录。

```
MediaAnalyse/
├─ CMakeLists.txt            顶层构建入口：C++20、子目录装配、Qt 探测时机
├─ CMakePresets.json         两个预设：win-mingw（CLI）/ win-mingw-app（GUI），写死便携工具链绝对路径
├─ README.md                 设计说明 + 行为契约 + 界面分区 + 进度
├─ RELEASE.md                发布说明与验收结果
├─ REFACTOR_PLAN.md          分阶段重构计划（P0–P5）与当前状态
├─ .gitignore                忽略 build/ dist/ 日志、崩溃转储、Python 缓存
│
├─ core/                     纯 C++20 内核（无 Qt、无 FFmpeg）—— 可被任何前端复用
├─ infra/                    FFmpeg 实现层（唯一接触 libav* 的地方）
├─ worker/                   崩溃隔离的分析进程（只链接 core+infra）
├─ bridge/                   Qt 适配层（唯一同时接触 core/infra 与 Qt 的地方）
├─ app/                      QML 界面 + 程序入口
├─ cli/                      无界面命令行分析器
├─ tests/                    单测 + Python 冒烟/集成/fuzz + 测试素材
├─ scripts/                  运维脚本（打包、一键诊断收集）
├─ docs/                     设计稿、截图、验证报告、本说明
└─ build/                    CMake 构建产物（已 gitignore，可随时删除重建）
```

### 2.1 各目录职责与关键文件

| 目录 | 包含内容 | 边界规则 |
|---|---|---|
| `core/` | `Result.hpp`（`Outcome`/`Diagnostic`：**错误即数据**）、`MediaInfo.hpp`（`AnalysisResult`/`StreamInfo`/`FrameRecord`/`StageTimings`）、`Analyzer.hpp`（分析接口 + `AnalysisOptions`）、`Alignment.hpp/.cpp`（**HLS 时间轴与对齐诊断**：playlist 解析、PDT 可选的三档时间基准、诊断算法、跨文件比较）、`UrlNormalizer`（`file:///C:/` → `file:C:/` 等 7 类修正）、`Serialization`（**JSON wire format 契约**：`toJson`/`fromJson`/`parseAnalyzeRequest`，含 `alignment`/`comparison` 段） | 不 include 任何 Qt / FFmpeg 头；不含 I/O 策略 |
| `infra/` | `Raii.hpp`（RAII）、`Interrupt.cpp`（`av_log` → 日志桥接 + 分阶段超时 + 协作取消）、`SliceType.hpp`（**同步 I/P/B 位流读取**：header-only、无 FFmpeg 依赖、逐包 slice header 解析）、`HlsTimeline.hpp/.cpp`（AVIO 读 m3u8 + `io_open` 分片归属追踪）、`ProbeAnalyzer.cpp`（open → find_stream_info → demux，可选解码）、`CrashHandler.cpp`（minidump + 异常过滤器）、`Logger.hpp/.cpp`（跨进程滚动文件日志 + 凭据脱敏 + 崩溃应急写入） | 只依赖 `core` + FFmpeg；不出现 Qt |
| `worker/` | `worker_main.cpp`：从 stdin 读一条 analyze 请求，向 stdout 流式写 progress、探测后的 `streams` 行与逐批 `frames` 行，最后写全量 result；独立线程监听 `cancel`；启动即装 CrashHandler | 只链接 `core`+`infra`，**不链接 Qt**；崩溃只死自己 |
| `bridge/` | `Bridge.hpp/.cpp`：`AnalysisController`（`QProcess` 拉起 worker、解析 JSON、看门狗 + 自动重启一次、取消、`liveUpdated` 增量发布）、`FrameTableModel`（多列 `QAbstractTableModel` + 文本过滤 + 可见行映射 + `rowsAppended` 增量插入） | 唯一允许同时接触内核与 Qt 的层；QML 只认这一层 |
| `app/` | `main.cpp`（`QApplication` + 命令行传参自动分析 + CrashHandler）、`qml/`（`Main.qml` 布局交互、`Theme.qml` 设计令牌单例、`PrismButton/SectionHead/MetricCell/StreamCard/FrameGrid` 组件） | QML 不直接碰 C++ 内核，全部经 `bridge` |
| `cli/` | `main.cpp`：参数解析、human / `--json` 两种输出、退出码约定 | 与 worker 共用同一 `Serialization`，输出格式不会漂移 |
| `tests/` | `ser_test.cpp`（序列化往返 + 信封 + timings `-1→null→-1`）、`slice_type_test.cpp`（同步 I/P/B 位流读取 21 项：合成 Annex-B/avcC/HEVC/MPEG-2 NAL + 畸形输入扫描，纯头文件无 FFmpeg 依赖）、`controller_test.cpp`（**无窗口**控制器头测）、`smoke_cli_json.py`（CLI JSON 回归 14 项）、`pict_type_test.py`（帧类型一致性 8 项，可直接喂文件 / RTSP 地址 / 可选 ffprobe 对照）、`gui_integration_test.py`（GUI+worker 隔离冒烟）、`summary_fold_test.py`（折叠结构 4 项 + 实机截图对比 1 项：自动定位折叠头、点击折叠、`Ctrl+Shift+S` 恢复、帧表行分隔符上移与行数增长）、`live_snapshot_test.py`（增量快照协议 17 项）、`live_stream_gui_test.py`（本地无限 TS 流端到端）、`fuzz_worker.py`（变异/截断 fuzz）、`log_smoke.py`（日志 17 项回归）、`alignment_test.cpp`（HLS 时间轴/对齐纯算法 40 项）、`alignment_cli_test.py`（四种真实 m3u8 清单端到端 12 项，样本缺失时用 ffmpeg 再生成）、`assets/sample.mp4`（4 s、320×176 H.264 + AAC 测试素材）、`manual/`（需本地推流环境的手工脚本：`ma_gui_check.py` 窗口截图/驱动、`ma_hls_test.py`、`ma_live_test.py`） | `ser_test` 只依赖 `ma_core`，`slice_type_test` 只 include 一个纯头文件；都不拖 FFmpeg/Qt；`controller_test` 仅在 `MA_ENABLE_APP=ON` 时构建 |
| `scripts/` | `package.py`：从构建树挑出运行时文件，产出一个干净的可分发目录；`collect-logs.cmd` + `collect_logs.ps1`：一键把日志 / minidump / 环境信息打包成 zip | 幂等；不修改构建树；收集脚本只用 Windows 自带组件 |
| `docs/` | `design/`（PRISM 设计稿：`design-preview.html`、`media-analyzer.qss`、参考截图、`qt_reference.py`）、`screenshots/`（成品界面 6 张 + `final_ui.png`）、`verification/`（fuzz 报告 JSON）、`PROJECT_GUIDE.md`（本文） | 纯文档资产，不参与构建 |

### 2.2 数据流（为什么这样分层）

```
             ┌────────────── GUI 进程（media-analyse.exe，含 Qt）──────────────┐
  QML  ──►  bridge::AnalysisController  ──QProcess(stdin/stdout, 行分隔 JSON)──┐
             │  看门狗：无 result 即判崩溃，最多重启一次，抓 minidump          │
             └───────────────────────────────────────────────────────────────┘
                                    │
             ┌──────────────────────▼──── worker 进程（analyzer-worker.exe，无 Qt）──┐
             │  core::parseAnalyzeRequest → infra::ProbeAnalyzer → core::toJson       │
             │  progress 行流式回传；崩溃只在本进程，写 crashes/<app>-<ts>.dmp        │
             └───────────────────────────────────────────────────────────────────────┘
```

`core::Serialization` 是唯一的协议实现：CLI `--json`、worker→GUI 的 `streams`/`frames`/`result` 行、`fromJson` 解析全部走它，因此**三方格式天然一致**，不会出现「CLI 能读、GUI 读不了」。`streams`/`frames` 由 `ProbeAnalyzer` 的可选 `SnapshotFn` 回调产出，CLI 不传该回调，所以命令行输出完全不受影响。

---

## 3. 构建

### 3.1 环境要求

| 组件 | 版本 / 路径（本机） | 说明 |
|---|---|---|
| 编译器 | GCC 16.2.0 `D:\CodeTools\mingw64\bin` | CLI / worker / 单测用它 |
| CMake | 4.4.4 `D:\CodeTools\mingw64\bin\cmake.exe` | 该目录不在默认 PATH，需用绝对路径或自行加入 PATH |
| Ninja | 1.13.2 同上 | 预设已指定 `CMAKE_MAKE_PROGRAM` |
| Qt | 6.10.3 `D:\CodeTools\Qt\6.10.3\mingw_64` | 仅 GUI 需要 |
| Qt 配套 MinGW | 15.1 `D:\CodeTools\Qt\Tools\mingw1510_64\bin` | **GUI 必须用它**：用 GCC 16 链接 Qt 会因 `__imp___argc` 等符号不兼容失败 |
| FFmpeg | 9.0.2 `D:\CodeTools\ffmpeg-n9.0-latest-win64-gpl-shared-9.0` | 开发包 + 运行时 DLL |

CMake 最低 3.24，语言标准 C++20。工具链路径全部写死在 `CMakePresets.json` 里，换机器只需改这一个文件。

### 3.2 两个预设

| 预设 | 编译工具链 | 产物 | 适用 |
|---|---|---|---|
| `win-mingw` | GCC 16.2.0 | CLI、worker、`ma_ser_test` | 只改内核 / 只想跑命令行 |
| `win-mingw-app` | Qt 配套 MinGW 15.1 | GUI + 以上全部 + `ma_controller_test` | 改界面、验证隔离链路、打包 |

```bash
# 只构建命令行与内核
cmake --preset win-mingw
cmake --build --preset win-mingw --parallel 8

# 构建完整 GUI（会连带构建 CLI、worker、全部测试）
cmake --preset win-mingw-app
cmake --build --preset win-mingw-app --parallel 8
```

### 3.3 产物落点（实测）

```
build/win-mingw/                     # 分层输出（非 Qt 目标）
├─ cli/media-analyse-cli.exe
├─ worker/analyzer-worker.exe        # 附带 FFmpeg + MinGW 运行时 DLL
└─ tests/ma_ser_test.exe

build/win-mingw-app/                 # 扁平输出 —— 整个目录 ≈ 绿色包
├─ media-analyse.exe                # GUI
├─ analyzer-worker.exe              # 分析内核（部署副本）
├─ media-analyse-cli.exe            # 命令行（同名预设顺带构建）
├─ ma_ser_test.exe / ma_controller_test.exe
├─ Qt6*.dll（27 个）+ FFmpeg DLL + MinGW 运行时
├─ qt.conf                          # Prefix=. （windeployqt 默认写 Prefix=..，构建后已改写）
├─ platforms/ imageformats/ styles/ tls/ qml/ ...   # Qt 插件与 QML 模块
└─ worker/ app/ core/ ...           # 中间产物，可忽略
```

> `build/win-mingw-app/` 之所以是扁平的：`qt_standard_project_setup()` 把运行时输出目录指向构建根，所以 Qt 目标（含测试）都落在这一层。**注意**：这里同时混有测试 exe 与日志，正式分发请用 `scripts/package.py` 出干净包（见 §4）。

### 3.4 构建注意

- **改 `app/qml/*.qml` 必须重新 build**：QML 被编译进资源（`qmlcachegen`），改文件不重编不生效。
- **链接报 `Permission denied`**：旧进程仍占用 exe，先 `taskkill /F /IM media-analyse.exe`（必要时再 `analyzer-worker.exe`）。
- **worker 改了但 GUI 目录里的副本没更新**：已修复——`app/CMakeLists.txt` 的 `ma_deploy_runtime`（`ALL` 目标）每次构建都对账拷贝 worker + FFmpeg/MinGW DLL，`copy_if_different` 保证无变化时零开销。
- **全量重建**：直接删 `build/` 即可，没有任何手写文件在里面（CMake 缓存含绝对路径，挪动目录后必须重新 configure）。
- `compile_commands.json` 由 `CMAKE_EXPORT_COMPILE_COMMANDS=ON` 生成在预设构建目录下，供 clangd 使用。

---

## 4. 打包

### 4.1 一条命令出包

```bash
python scripts/package.py                       # -> dist/MediaAnalyse-v1.0
python scripts/package.py --with-cli            # 附带 media-analyse-cli.exe
python scripts/package.py --out dist/test --force
```

脚本从 `build/win-mingw-app/` 中**挑选**运行时所需文件，产出干净目录：

```
dist/MediaAnalyse-v1.0/
├─ media-analyse.exe           GUI（双击即用）
├─ analyzer-worker.exe         必须与 GUI 同目录（按 applicationDirPath 定位）
├─ Qt6*.dll / av*-*.dll / libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll
├─ qt.conf                    Prefix=. （缺了会导致 QML 模块找不到）
├─ platforms/ imageformats/ styles/ tls/ qml/ ...    Qt 插件与 QML 导入
├─ README.md
└─ PROJECT_GUIDE.md
```

**不会**打进包里：`ma_ser_test.exe`、`ma_controller_test.exe`、`*.log`、`crashes/`、`.pdb/.lib/.a/.obj`、CMake 中间目录。

### 4.2 打包原理与注意

- Qt 依赖由 `windeployqt --qmldir app/qml --no-translations` 在**构建时**部署（`app/CMakeLists.txt` 的 POST_BUILD），只拷真正用到的 DLL / 插件，不整目录搬运。
- `qt.conf` 由构建后自动改写为 `Prefix=.`：windeployqt 默认按 `bin/` 布局写 `Prefix=..`，而我们的 exe 就在包根，不改会加载不到插件。
- FFmpeg 只拷 `avcodec/avformat/avutil/swscale/swresample` 这 5 类 DLL（`MA_FFMPEG_RUNTIME_DLLS`），不带 `ffmpeg.exe` / `ffplay.exe` / `ffprobe.exe`。
- 分发前建议实测：把 `dist/MediaAnalyse-v1.0` 整体拷到**没有安装 Qt/FFmpeg 的目录或机器**，双击 `media-analyse.exe` 打开一个本地文件——能出流卡片与帧表即部署完整。
- 授权提醒：FFmpeg 为 **GPL** 构建，本项目仅内部排障使用，不对外分发。

---

## 5. 使用说明

### 5.1 GUI

启动：双击 `media-analyse.exe`，或命令行带参直接分析：

```bash
media-analyse.exe D:/videos/a.mp4
media-analyse.exe "rtsp://127.0.0.1:8554/live"
```

界面逻辑上分 7 个区块：APP BAR（品牌 / 状态 / 配置）→ SOURCE（本地文件 / 网络流）→ SUMMARY（**一个可折叠分组**：折叠头 + OVERVIEW 5 指标 + STREAMS 流卡片 + ALIGNMENT 对齐诊断折叠头）→ NOTICE（三态提示条，可关闭）→ TOOLBAR（视频/音频 Tab + 过滤 + 复制）→ DATAVIEW（帧表）→ STATUS（提示 / 选区 / 行数）。实际视觉顺序中 NOTICE 位于 OVERVIEW 与 STREAMS 之间，它属于 SUMMARY 容器但**不随折叠隐藏**。

关键操作：

| 操作 | 行为 |
|---|---|
| 「打开」 | 弹原生文件对话框；**选完立即分析**，无需再点一次 |
| 「解析」（网络流） | 协议由地址自动判定（`AUTO · RTSP` 只读徽标）；实时流显示 `LIVE` |
| 「停止解析」 | **中性操作**：不弹错误框，保留已解析的部分帧并标注停止在第几个包 |
| 帧表拖选 | 矩形选区（点 `#` 列选中整行，`Shift` 扩展） |
| `Ctrl+C` / 「复制选区」 | 按 TSV 写入剪贴板，可直接粘进 Excel |
| 过滤框 | 按任意列文本筛选；Tab 行数与状态栏同步 |
| 滚轮 | 上下滚动（一格 3 行），`Shift` + 滚轮横向 |
| 「SUMMARY / 概览与流信息」折叠头 | 点击整行或按 `Ctrl+Shift+S`：一次性收起 / 展开 OVERVIEW 指标条与 STREAMS 流卡片，DATAVIEW 因 `Layout.fillHeight` 自动补满腾出的高度（约 230–350 px，实测可见帧行 6 → 26）。收起后折叠头仍显示「来源 · N 条流 · M 包」摘要；NOTICE 提示条常显。窗口高度 <780 逻辑像素且未手动切换过时自动折叠，手动切换后以用户选择为准 |
| 「ALIGNMENT / 对齐诊断」折叠头 | 分析完成后出现，默认只占一行（时间基准 + 错误/警告数），点击展开墙上时钟起止、每条流的起始偏移 / 时钟漂移 / 最大空档与问题列表。无 PDT 显示「相对时间轴」；有 PDT 显示「PDT 绝对时钟」 |
| 「配置」 | 连接 / 无数据 / 探测三个超时、最大帧数、最大解码帧数、RTSP over TCP、自动重连、`K=V` 高级选项；**下次分析生效** |
| 提示条「详情」 | toast 展示完整诊断：错误码 / 阶段 / 原生码 / 地址 / `open Xs · probe Ys · demux Zs` |

帧表 `type` 列同步读取**每个 packet 自己的 slice header**（H.264/H.265 `slice_type`、MPEG-1/2 `picture_coding_type`、全 I 编码直通 `I`），因此 `I` / `P` / `B` 都与该行的时间戳严格对应；`AV_PKT_FLAG_KEY` 作为权威兜底，保证真 IDR 不会被误标成 `P`。无法识别的码流才显示 `-`。概览「关键帧」独立按 `keyframe` 计数，不受 P/B 标签影响。**不用 `AVCodecParserContext`、也不用解码器输出**：`av_parser_parse2()` 吐出的是上一帧且 `pict_type` 会保持到下一帧被识别，贴到当前包行会造成帧类型与时间戳错配（实测 1 s GOP 的 RTSP 流出现「开头 3 个 I、之后每 GOP 2 个 I」）；增量快照行一经发布也无法事后回填。实现见 `infra/include/ma/infra/SliceType.hpp`，回归见 `tests/slice_type_test.cpp` / `tests/pict_type_test.py`。

**实时流（无 EOF）行为**：直播地址不会自然结束，因此数据不是「分析完才显示」。探测（`find_stream_info`）一结束就推送一次 `streams` 快照，STREAMS 卡片与概览立即出现；demux 期间每累计 128 包且距上批 ≥0.25 s 推送一批 `frames`，GUI 用 `beginInsertRows` 增量追加帧行，**不清除选区、过滤框和滚动位置**，状态保持 `LIVE · 帧解析中`。点「停止解析」或达到 `maxFrames` 后，worker 发出最终 `result`，界面按完整结果覆盖一次并显示部分结果提示。若长时间停在「解析中」但表格 0 行，先看 `logs/media-analyse.log` 是否出现 `live streams snapshot` / `live frames batch`：没有 `streams` 说明还卡在连接/探测阶段，有 `streams` 无 `frames` 说明源端没有持续送包。

帧表列按 Tab 自适应（各 13 列）：

- 视频：`# pkt# stream dts dts-time dts-delta pts pts-time duration size type dims format`
- 音频：`# pkt# stream dts dts-time dts-delta pts pts-time duration size samples format ch`

`type` 由当前 packet 的 slice header 同步解析（只读帧头，不解码像素、不使用滞后的 FFmpeg parser）；`dims/format/ch` 是**解码专属列**，勾选「解码」后为每条流前 `maxDecodeFrames` 帧补充，且**不改变行数/时间戳/type/samples**。

### 5.2 CLI

```bash
media-analyse-cli [--json] [--frames[=N]] [--decode] \
                  [--max-frames N] [--max-decode-frames N] \
                  [--probesize B] [--analyze-duration US] \
                  [--connect-timeout S] [--stall-timeout S] [--probe-timeout S] \
                  [--opt K=V] [--no-align] [--compare <file>] [--anchor-ms N] \
                  <url|file>
```

| 选项 | 说明 |
|---|---|
| `--json` | 输出单个 JSON 对象（成功/失败同一信封） |
| `--frames[=N]` | 附带前 N 行帧；不带 `=N` 表示全部 |
| `--decode` | 真实解码，补 `dims/format/ch`（每条流独立预算） |
| `--max-frames N` | 最多读多少包（默认上限见 `AnalysisOptions`） |
| `--max-decode-frames N` | 每条流最多解码多少帧（默认 3000） |
| `--connect-timeout / --stall-timeout / --probe-timeout` | 连接 / 逐包无数据 / `find_stream_info` 三阶段独立超时（秒） |
| `--opt K=V` | 透传 FFmpeg 选项，可重复：`--opt rtsp_transport=tcp` |
| `--no-align` | 跳过 HLS 时间轴 / 对齐后处理，只输出原始包 |
| `--compare <file>` | 再分析一份录制文件，输出 `comparison`（两份都有 PDT 时为精确墙上时钟偏移） |
| `--anchor-ms N` | 无 PDT 时给 `--compare` 指定人工同步点（毫秒，正值=主文件更晚，置信度 0.5） |

对齐诊断详见 README §「HLS 录制文件对齐诊断（PDT 可选）」。选项可以放在主输入之前或之后；未加引号、含空格的 Windows 路径仍会被自动拼接。

退出码：`0` 成功，`1` 分析失败（stderr 有结构化诊断），`2` 用法错误。

JSON 信封（成功路径，节选）：

```json
{
  "ok": true,
  "diagnostic": {"code":"ok","stage":"none","nativeCode":0,"message":"","elapsed":0.0066},
  "error": null,
  "timings": {"open":0.0028,"probe":0.0029,"demux":0.0009,"total":0.0066},
  "format": {"name":"mov,mp4,m4a,3gp,3g2,mj2","duration":4.0,"bitrate":799154},
  "streams": [ ... ],
  "frames": [ ... ],
  "notes": "decode: 40 frames; ...",
  "ffmpeg": "9.0.2"
}
```

失败路径 `ok=false`，`diagnostic` 给出 `{code, stage, nativeCode, message, hint, url}`，未到达阶段的 `timings` 为 `null`。脚本可只依赖 `ok` + `diagnostic.stage` 做断言。

### 5.3 崩溃与 minidump

分析内核跑在 `analyzer-worker.exe` 里，**FFmpeg/解码器崩溃不会带走主窗口**：

- 崩溃时在 `<exe 所在目录>/crashes/<app>-<ts>.dmp` 写 minidump，并在 stderr 打一行 `[CRASHDUMP] <绝对路径>`；
- 看门狗发现「进程退出但没有 result 行」→ 自动重启 worker **一次**；仍失败则界面内联报错并附 dmp 路径；
- 用 WinDbg / Visual Studio 打开 `.dmp` 可看崩溃线程与调用栈；把复现输入交给 `tests/fuzz_worker.py --corpus-dir` 可做单步回归。

### 5.4 运行日志与诊断收集

打包版 GUI 是 WIN32 子系统程序，**没有控制台**：Qt/QML 警告与 FFmpeg 诊断默认全部不可见。为解决「用起来出问题却拿不到任何线索」，三个进程各自写一份滚动 UTF-8 日志：

| 文件 | 记录内容 |
|---|---|
| `logs/media-analyse.log` | 启动/退出、exe 路径与工作目录、命令行参数、Qt 消息处理器捕获的 **QML/Qt 警告与错误**、控制器 start/cancel、worker PID、`stderr`、result、退出码、崩溃与自动重启 |
| `logs/analyzer-worker.log` | 每次请求（输入 URL、选项）、FFmpeg `av_log`、progress、阶段耗时、最终诊断、`result delivered, exiting` |
| `logs/media-analyse-cli.log` | CLI 调用参数、FFmpeg 版本、分析结果 / 失败诊断 |

**落盘位置**（按优先级选第一个可写目录）：`MA_LOG_DIR` → `<exe 所在目录>/logs`（打包时已预建）→ `%LOCALAPPDATA%\MediaAnalyse\logs` → `%TEMP%\MediaAnalyse\logs`。装到只读目录（如 Program Files）时自动回退到用户目录，不会静默丢日志。

**格式**：`2026-11-06 15:59:44.569 [INFO ] [p23368/t13616] [logger] ...`，带毫秒时间戳、级别、进程/线程号、组件名，一条一行（换行/制表符被转义）。每个进程一个文件，天然避免多进程加锁；Windows 用 `FILE_APPEND_DATA` 追加写、每条即时落盘，因此 worker 的 `std::_Exit()` 或异常崩溃不会丢最后几行。崩溃过滤器另外用无锁的 `emergencyLog` 补一条 `FATAL` + minidump 路径。

**滚动与级别**：默认单文件 5 MB、保留 5 代（`*.1.log` … `*.5.log`）。

| 环境变量 | 说明 |
|---|---|
| `MA_LOG_DIR` | 显式指定日志目录 |
| `MA_LOG_LEVEL` | `debug`（含 FFmpeg verbose） / `info`（默认） / `warn` / `error` / `off` |
| `MA_LOG_MAX_BYTES` | 滚动阈值，默认 `5242880` |
| `MA_LOG_MAX_FILES` | 保留代数，默认 `5` |

`off` 走零磁盘写入的快速路径，`tests/fuzz_worker.py` 默认设置它，万次 fuzz 不会因日志变慢或撑爆磁盘。

**脱敏**：URL 的 `user:pass@` 与 `token=` / `password=` / `key=` / `signature=` / `auth=` / `session=` / `api_key=` 等查询参数在写入前一律替换为 `***`（`ma::infra::redact`）。**本地文件路径与主机名不脱敏**，对外分享前请自行审阅。

**排查入口**：GUI 顶栏「**日志**」按钮直接打开日志目录；出问题时双击包内 `collect-logs.cmd`，它会在同目录生成 `MediaAnalyse-diagnostics-<时间戳>.zip`，内含全部滚动日志、最近 3 个 minidump 和 `info.txt`（采集时间、包路径、exe 构建时间、OS、用户、扫描过的目录清单）。把这个 zip 附到问题单即可。脚本只用 Windows 自带的命令行工具，不需要 Python / Qt / 管理员权限。

> 回归验证：`python tests/log_smoke.py`（17 项，覆盖三进程落盘、脱敏、`off` 零写入、滚动代数、包内日志目录可写）。

### 5.5 命令行与 GUI 的对应关系

| GUI 动作 | CLI 等价写法 |
|---|---|
| 打开文件并解析 | `media-analyse-cli a.mp4` |
| 勾选「解码」 | `--decode` |
| 配置里「最大帧数」 | `--max-frames N` |
| 配置里「最大解码帧数」 | `--max-decode-frames N` |
| 配置里三个超时 | `--connect-timeout/--stall-timeout/--probe-timeout` |
| 高级选项 `rtsp_transport=tcp` | `--opt rtsp_transport=tcp` |

---

## 6. 测试与验证

```bash
# 单元测试（无 FFmpeg、无 Qt，秒级）：序列化 + 同步 I/P/B 位流读取 + HLS 对齐算法
ctest --test-dir build/win-mingw --output-on-failure

# 控制器头测：controller → QProcess → worker → JSON → fromJson → analysisSucceeded
build/win-mingw-app/ma_controller_test.exe tests/assets/sample.mp4

# CLI JSON 契约回归（14 项，改 CLI/serialization 后必跑）
python tests/smoke_cli_json.py tests/assets/sample.mp4

# 帧类型一致性（8 项）：key=I、非 key 保留 P/B、无启动 I 爆发、每 GOP 一个 I、解码纯附加
python tests/pict_type_test.py
python tests/pict_type_test.py rtsp://127.0.0.1:8554/live --stall-timeout 5
python tests/pict_type_test.py build/rtsp_4s.ts --ffprobe  # 与 ffprobe 解码直方图对照

# GUI + worker 隔离冒烟（需 Qt 运行时 DLL 在 PATH 上）
python tests/gui_integration_test.py
python tests/gui_integration_test.py --app dist/MediaAnalyse-v1.0/media-analyse.exe  # 验证打包结果

# 概览折叠（结构 4 项 + 实机截图对比，需 Qt 运行时 DLL 在 PATH 上）
python tests/summary_fold_test.py
python tests/summary_fold_test.py --app dist/MediaAnalyse-v1.0/media-analyse.exe

# worker 增量快照协议（17 项：streams/frames 顺序、批次不重叠、result 仍完整）
python tests/live_snapshot_test.py

# 无限流端到端：本地循环推 MPEG-TS，验证「解析中」时流信息与帧行持续增长
python tests/live_stream_gui_test.py
python tests/live_stream_gui_test.py --app dist/MediaAnalyse-v1.0/media-analyse.exe

# 日志回归（17 项：落盘 / 脱敏 / 滚动 / off / 包内可写）
python tests/log_smoke.py

# HLS 对齐诊断端到端（12 项：有/无 PDT、缺分片、漏标 discontinuity、IPC、三种比较模式）
python tests/alignment_cli_test.py

# fuzz：随机变异 + 截断，1 万次喂 worker
python tests/fuzz_worker.py --exe build/win-mingw-app/analyzer-worker.exe \
                            --seed tests/assets/sample.mp4 --mode worker --iters 10000
```

判定标准：`ctest` 三个测试（`ser_test` / `slice_type_test` / `alignment_test`）全绿，`alignment_test` 打印 `40/40 checks passed`；`smoke_cli_json.py` 必须 `14/14 checks passed`；`pict_type_test.py` 必须 `8/8 checks passed`；`live_snapshot_test.py` 必须 `17/17 checks passed`；`live_stream_gui_test.py` 必须 `LIVE STREAM GUI: PASS`；`summary_fold_test.py` 必须 `5/5 checks passed`；`log_smoke.py` 必须 `17/17 checks passed`；`alignment_cli_test.py` 必须 `12/12 checks passed`；fuzz 必须 `crash=0 / timeout=0 / abnormal=0`（`clean_fail` 是畸形输入被转成结构化诊断，属预期）。历史报告见 `docs/verification/`。

需要真实推流环境的手工脚本在 `tests/manual/`（需自备 `mediamtx` 与 ffmpeg 推流）：

```bash
python tests/manual/ma_live_test.py     # RTSP / RTMP / HLS 端到端 + GUI 停止
python tests/manual/ma_hls_test.py      # HLS 专项（验证探测阶段超时）
python tests/manual/ma_gui_check.py --exe build/win-mingw-app/media-analyse.exe --shot out.png
```

---

## 7. 排障速查

| 现象 | 原因 / 处理 |
|---|---|
| `cmake: command not found` | `D:\CodeTools\mingw64\bin` 不在 PATH，用绝对路径调 `cmake.exe` |
| 链接报 `Permission denied` | 旧 `media-analyse.exe` 仍在跑：`taskkill /F /IM media-analyse.exe` |
| Qt 链接报 `__imp___argc` 等符号错误 | 用了 GCC 16 编 GUI，改用 `win-mingw-app` 预设（Qt 配套 MinGW 15.1） |
| 双击 exe 无窗口 / 提示缺 DLL | 缺 `platforms/qwindows.dll` 或 `qt.conf` 被改坏：重新 build 或重新 `scripts/package.py` |
| 界面文字/图标异常但功能正常 | 远程桌面/老显卡：设 `QT_QUICK_BACKEND=software` 走软件渲染 |
| 使用中出现异常但没有任何线索 | 先看 `logs/*.log`（GUI 顶栏「日志」按钮可直接打开目录）；双击 `collect-logs.cmd` 生成诊断 zip 回传 |
| 日志目录为空 / 找不到日志 | 可能装在只读目录，检查 `%LOCALAPPDATA%\MediaAnalyse\logs` 与 `%TEMP%\MediaAnalyse\logs`；也可用 `MA_LOG_DIR` 显式指定 |
| 分析无结果、GUI 报 dmp 路径 | 内核崩溃已被隔离，看 `crashes/*.dmp`；用 fuzz 复现输入回归 |
| 网络流「连不上」 | 先确认 FFmpeg 能拉：`ffprobe <url>`；再试 `--opt rtsp_transport=tcp`、加大 `--probe-timeout` |
| 改了 QML 没变化 | QML 编进资源，必须重新 build |
