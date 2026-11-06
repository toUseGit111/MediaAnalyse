# media-analyse

MediaAnalyse 的重写版本：面向内部排障的媒体分析工具。

- **内核**：纯 C++20（无 Qt 依赖），FFmpeg **9.0.2 "Lei"**（libavcodec 63 / libavformat 63）
- **前端**：Qt Quick / QML（Qt 6.10.3 + MinGW 15.1）；QtWidgets 仅用于原生文件对话框
- **构建**：CMake ≥ 3.24 + Ninja，便携工具链，无安装程序
- **定位**：内部使用，不对外分发（FFmpeg 使用 GPL 构建，仅在内部排障场景使用）

## 目录结构

```
CMakeLists.txt   顶层构建入口（C++20、子目录装配、Qt 探测时机）
CMakePresets.json win-mingw（CLI）/ win-mingw-app（GUI）两个预设，工具链路径写死
core/    纯 C++20，无 Qt / 无 FFmpeg：Result/Diagnostic、URL 规范化、数据结构、Analyzer 接口、JSON 序列化契约（Serialization）
infra/   FFmpeg 实现：RAII 封装、av_log 桥接、分阶段超时中断、ProbeAnalyzer、崩溃处理（CrashHandler：minidump + 异常过滤器）
worker/  崩溃隔离的分析内核（无 Qt）：从 stdin 读分析请求、流式 progress + 最终 result 到 stdout；崩溃只杀本进程并写 minidump
bridge/  Qt 适配层：AnalysisController 通过 QProcess 拉起 worker、解析其 JSON、看门狗自动重启一次、原生文件选择器；FrameTableModel（多列）
app/     QML 界面 + 程序入口（含 CrashHandler）
cli/     media-analyse-cli：无界面命令行分析器（回归测试与脚本化排障用，同样装 CrashHandler）
tests/   单测（ser_test / controller_test）+ Python 冒烟（CLI JSON 回归、GUI 集成、fuzz 变异）+ assets/sample.mp4 + manual/（需推流环境的手工脚本）
scripts/ package.py：从构建树挑出运行时文件，产出干净的可分发目录
docs/    design/（PRISM 设计稿）、screenshots/（成品截图）、verification/（fuzz 报告）、PROJECT_GUIDE.md（工程说明）
build/   CMake 构建产物（已 gitignore，可随时删除重建）
```

> 目录职责、编译 / 打包 / 使用细节见 [`docs/PROJECT_GUIDE.md`](docs/PROJECT_GUIDE.md)。

## 构建

工具链（已在本机 `D:\CodeTools` 就位，均为绿色解压，无 UAC）：

| 组件 | 路径 |
|---|---|
| GCC 16.2.0 + CMake 4.4.4 + Ninja 1.13.2 | `D:\CodeTools\mingw64\bin` |
| Qt 6.10.3 (mingw_64) | `D:\CodeTools\Qt\6.10.3\mingw_64` |
| Qt 配套 MinGW 15.1（app 必须用它） | `D:\CodeTools\Qt\Tools\mingw1510_64\bin` |
| FFmpeg 9.0.2 开发包 | `D:\CodeTools\ffmpeg-n9.0-latest-win64-gpl-shared-9.0` |

```bash
# 只构建 CLI（GCC 16 工具链）
cmake --preset win-mingw
cmake --build --preset win-mingw

# 构建 CLI + QML 应用（Qt 配套工具链）
cmake --preset win-mingw-app
cmake --build --preset win-mingw-app
```

产物：

- `build/win-mingw/cli/media-analyse-cli.exe`、`build/win-mingw/worker/analyzer-worker.exe`（分层输出，FFmpeg + MinGW 运行时 DLL 已拷到同目录）
- `build/win-mingw-app/` 是**扁平**输出（`qt_standard_project_setup` 把运行时目录指到构建根）：`media-analyse.exe`、`analyzer-worker.exe`、`media-analyse-cli.exe`、Qt DLL + `platforms/`/`qml/` 插件、`qt.conf` 全在这一层，整个目录即可运行
- 正式分发不要直接拷该目录（里面混有测试 exe 与日志），用 `python scripts/package.py` 产出干净包到 `dist/MediaAnalyse-v1.0/`

## CLI 用法

```bash
media-analyse-cli [--json] [--frames[=N]] [--decode] \
                  [--max-frames N] [--max-decode-frames N] \
                  [--probesize B] [--analyze-duration US] \
                  [--connect-timeout S] [--stall-timeout S] [--probe-timeout S] \
                  [--opt K=V] <url|file>
```

示例：

```bash
media-analyse-cli --frames=5 C:/videos/a.mp4
media-analyse-cli --json "file:///C:/videos/a.mp4"     # 三斜杠写法自动修正
media-analyse-cli --decode --opt rtsp_transport=tcp rtsp://cam/stream
```

退出码：`0` 成功，`1` 分析失败（stderr 输出结构化诊断），`2` 用法错误。

### JSON 回归检查

`--json` 保证输出可被标准 JSON 解析器读取，包括 `--frames=N` 截断行数、分析失败路径，以及**解码路径的纯附加契约**（行数/时间戳/I-P-B 不因 `--decode` 改变、`--max-decode-frames` 生效、帧级 extras 有值、**超预算时前部行仍保留 extras 且各流都有**）。成功时顶层 `notes` 会带上解码统计 / URL 修正 / 停止提示，便于脚本断言。改动 CLI 后跑一次（纯标准库，自动生成本地 wav / 垃圾文件 / 不存在的路径）：

```bash
python tests/smoke_cli_json.py                     # 7 项：JSON 合法性 + 错误路径 + 解码纯附加
python tests/smoke_cli_json.py C:/videos/a.mp4     # 再 +7 项：视频 I/P/B、samples、extras、预算上限/前部保留/按流预算/提示语
```

可用 `MA_CLI` 指定其他 CLI 路径；两个 preset 均跑出 `14/14 checks passed` 即通过。

## 界面（PRISM 设计系统）

按 `docs/design/` 的设计稿 1:1 移植到 Qt Quick（QML）。设计令牌集中在
`app/qml/Theme.qml`（单例），组件在 `PrismButton.qml` / `SectionHead.qml` /
`MetricCell.qml` / `StreamCard.qml` / `FrameGrid.qml`，布局与交互在 `Main.qml`。
深色精密仪器风格：琥珀色只用于主按钮、激活 Tab、I 帧、警告和焦点；其余为暖调石墨中性色。
Qt Quick Controls 强制使用 **Basic** 风格，避免系统/Material 配色渗入自定义背景。

从上到下的阅读顺序：

| 区块 | 内容 |
|---|---|
| 01 APP BAR | 品牌 / 全局状态标签 / 「配置」 |
| 02 SOURCE | 「本地文件 / 网络流」分段切换；**唯一主按钮**（打开 / 解析）；只读协议徽标 `AUTO · RTSP`；LIVE 状态行与**独立的「停止解析」按钮** |
| 03 OVERVIEW | 5 个等宽指标：时长 / 平均码率 / 包总数 / 解析耗时 / 容器格式；切到网络流后变为 LIVE / 包数 / 帧数 / 接入协议 |
| 04 NOTICE | 可关闭的三态提示条（琥珀警告 / 蓝色信息 / 红色错误），「详情」用 toast 显示完整诊断 |
| 05 STREAMS | 视频（蓝顶边）/ 音频（橙顶边）卡片 + 4 列属性网格 |
| 06 TOOLBAR | 视频 / 音频 Tab 带行数徽标、过滤框、复制选区 |
| 07 DATAVIEW | 吸顶表头、斑马纹、I/P/B 色标、矩形选区、每 Tab 13 列（视频/音频各一套） |
| 08 STATUS | 操作提示渐进披露 + 选区信息 + 行数 + 状态圆点 |

**网络流只要求一个地址**：`rtsp(s)://` → RTSP、`rtmp*://` → RTMP、`.m3u8` → HLS、
`.flv` → HTTP-FLV 由地址自动判定，结果是只读徽标而非控件；未识别时仍可交给 FFmpeg 尝试
（内部排障不阻断未知协议）。主界面**不**暴露传输/缓冲/认证/超时/重连控件。

## GUI 操作

- SOURCE 区右上角分段切换「本地文件 / 网络流」，切换后概览指标与提示条语义同步变化。
- **文件模式**：点「打开」弹出 Windows 原生文件对话框（媒体文件 / 视频 / 音频 / 所有文件四类过滤器，记住上次目录）。选中后路径自动回填并**立即开始分析**，无需二次点击；点「取消」不启动分析。「浏览…」只回填路径不启动。也可直接粘贴路径后按回车。
- **URL 模式**：粘贴地址后点「解析」或按回车；空地址不会误触发。
- 分析过程中主按钮禁用，**独立的「停止解析」按钮**负责取消。停止是**中性操作**：不弹任何框、不把取消当错误；已解析的帧（含解码 extras）作为**部分结果**显示，琥珀提示条写明停止在第几个包并停止读取后续帧；0 帧时保留上次结果。真实失败改为**内联红色提示条 + 状态栏错误态**，不再有模态错误弹窗。
- 「配置」里的连接超时、无数据超时、**探测超时**、最大帧数、最大解码帧数在下一次分析时生效；**FFmpeg 高级选项**按 `K=V`（逗号 / 分号 / 换行分隔）透传，附「RTSP over TCP」「自动重连」快捷预设，例如 `rtsp_transport=tcp, reconnect=1`。未被 FFmpeg 使用的选项会在提示条里以 `unused options: ...` 反馈。
- 工具栏「过滤」按任意列的文本筛选可见行，Tab 计数与状态栏行数同步变化；「复制选区」导出 TSV 后底部 toast 提示。
- 也支持命令行带参启动后自动分析：`media-analyse.exe C:/videos/a.mp4`（URL 参数会自动切到网络流模式）。

### 帧表

列顺序按 Tab 自动适配（**均 13 列**，只显示与该流类型相关的列）：

- **视频 Tab**：`#`（当前 Tab 内帧号）、`pkt#`（文件内全局包序号）、`stream`、`dts`、`dts-time`、`dts-delta`、`pts`、`pts-time`、`duration`、`size`、`type`、`dims`、`format` —— 不含 `samples` / `ch`。
- **音频 Tab**：`#`、`pkt#`、`stream`、`dts`、`dts-time`、`dts-delta`、`pts`、`pts-time`、`duration`、`size`、`samples`、`format`、`ch` —— 不含 `type` / `dims`。

- `type`：默认由 FFmpeg 码流 parser 解析出 **I / P / B / S**（只解析帧头、不解码像素，速度接近纯 demux）；`samples` 由音频 packet duration 按采样率换算（AAC 通常 1024），视频帧显示 `-`。
- **`dims` / `format` / `ch` 是解码专属列**：`dims` 为 `宽x高`（仅视频 Tab 有此列），`format` 为像素格式（视频）或采样格式（音频），`ch` 为声道数（仅音频 Tab 有此列）。未勾选「解码」、或该包没解出帧时显示 `-`。
- **勾选「解码」是纯附加操作**：为**每条流的前 `maxDecodeFrames` 帧**（默认 3000）补上面三列，**不会改变行数、`pkt#`、时间戳、`type` 或 `samples`**。刻意不从解码器取帧类型/采样数——B 帧重排下第 N 个包并不产出第 N 个显示帧，旧实现曾因此把 I/P/B 与 pts 错配。
- **超出解码预算时保留前若干帧信息**：预算用尽后后续行只是退回包级（末三列 `-`），**已解出的头部行仍保留 `dims`/`format`/`ch`**。黄色提示行明确写出 `frame-level dims/format/ch ARE shown on the first N rows (maxDecodeFrames=... per stream); later rows stay packet-level only`。按流计数可避免音频包密集的容器把全局配额吃光、让视频 Tab 整页都是 `-`。每次分析完成（含部分结果）表格都会自动回到第 1 行第 1 列，确保这些头部行一打开就可见。
- 分析结束会 flush 解码器，提示行显示 `decode: N frames + M tail frames flushed`；解码器有启动延迟，开头几个包可能仍为 `-`（本样本 250 个视频包中 248 行有 extras）。
- **矩形单元格选区**（与旧版 `QTableWidget` 一致）：在表格里**按住鼠标左键拖出一片矩形**，覆盖到的单元格整块蓝底白字高亮；点 `#` 列则一次选中整行（当前 Tab 的全部列），`Shift` + 点/拖可从锚点扩展选区。切换 Tab 或重新分析会清空选区。
- 按 `Ctrl+C` 或点「复制选区」把高亮区域按 **TSV（纯数据、无表头）** 写入剪贴板，可直接粘进 Excel；状态栏显示复制的行数 × 列数。
- 表格本身**固定不随鼠标拖动位移**（`TableView.interactive: false`）；**滚轮上下滚动**（一格 3 行），`Shift` + 滚轮横向滚动，右侧滚动条仍可直接拖动。滚轮由覆盖层自行改写 `contentY/contentX` 实现，因为 `interactive:false` 会一并关掉 Flickable 自带的滚轮处理。切换 Tab / 重新分析会清空选区并把滚动位置归零。工具栏过滤在 `FrameTableModel` 内部用「可见行 → 源行」映射实现，因此选区 / 复制拿到的始终是屏幕上看到的行。

## 已实现的关键行为

- **URL 规范化**：`file:///C:/x` → `file:C:/x`（FFmpeg 在 Windows 下不接受三斜杠盘符形式，这是旧版「不支持」的主因之一）；去引号/空白；反斜杠转换；未知 scheme 原样透传并标注。
- **多流模型**：任意路 video/audio/subtitle/data/attachment 都被列出，不再假设「一路视频 + 一路音频」。
- **元数据优先 + 轻量帧类型**：默认只 demux，用 `AVCodecParserContext` 解析 I/P/B、用 packet duration 换算音频采样数，不解码像素；`--decode` 才做真实解码（**每条流最多前 `maxDecodeFrames` 帧**），且**只附加帧级几何/格式 extras**，严格保持「一行 = 一个包」，避免 B 帧重排导致的类型与时间戳错配；超预算不丢弃已有 extras，结束时 flush 解码器并统计尾帧。
- **帧级 extras 可观测**：表格 `dims`/`format`/`ch` 三列、CLI human 输出的 `pix=`/`sfmt=`/`ch=`、CLI JSON 的 `width`/`height`/`pixFmt`/`sampleFmt`/`channels`/`decoded` 字段。
- **诊断面板阶段耗时**：`core::StageTimings` 记录 open / find_stream_info / demux 三段各自耗时与 total（未到达的阶段为 `-1`，序列化为 `null`）。CLI human 输出 `timings` 行、`--json` 输出 `timings:{open,probe,demux,total}`、GUI「详情」toast 展示 `open Xs · probe Ys · demux Zs`；失败时仍保留已发生阶段的耗时（例如 open 超时会显示 open 用时、probe/demux 为 `—`），把「慢在哪一步」直接摊开。
- **三阶段超时**：连接 / 逐包无数据 / **探测（find_stream_info）** 各自独立。探测阶段单独用 `probeSeconds`（默认 `max(60 s, 4×stall)`）兜底，避免慢速 HLS 直播 / 高延迟 RTSP 在探测时因为「没有新包」被误判为 stall 超时；`--probe-timeout S` 或 GUI 配置可显式指定。`interrupt_callback` 在 `avformat_open_input` **之前**挂上，慢速 HTTPS 连接也能立即取消/超时。
- **进度与中性取消**：每 64 包上报进度；用户停止不产生错误诊断、不弹失败框——有帧则发布部分结果并停止读取后续帧，无帧则保留上次结果。`stopAtCompletion` 在工作线程返回瞬间快照停止意图，避免「结果已排队、用户才点停止」把真实错误吞掉；硬错误（文件不存在 / 参数非法 / 协议不支持 / 内部错误）永不被取消语义掩盖。直播流服务器断开返回 `AVERROR_EOF` 且已点停止时同样标为部分结果，不会显示成 `Done`。
- **停止 / 打开不再共用一个按钮**：主按钮在 busy 时禁用，独立的「停止解析」只在 busy 时出现；`AnalysisController::cancel()` 在非 busy 时是 no-op，消除了点晚导致「打开」变「重新分析」的竞态。
- **结构化错误**：每次失败都有 `{code, stage, nativeCode, av_strerror 文本, hint, url}`，界面以内联提示条 + 状态栏展示，不再弹模态框。
- **协议自动识别 + 高级选项**：QML 按 URL 前缀/扩展名判定 RTSP / RTMP / HLS / HTTP-FLV；GUI「配置」可传任意 `K=V` 给 FFmpeg（`rtsp_transport=tcp`、`reconnect=1` 等），CLI 等价写法为 `--opt K=V`。
- **本地实测**：用 MediaMTX + FFmpeg 推流验证 `rtsp://` / `rtmp://` / `.m3u8` 均解析成功（各 120 包 / 2 条流）；GUI 对本地 RTSP 实时流点「停止解析」得到 `已停止解析 · 仅显示已解析的前 379 个包`，无弹窗、表格有部分行。
- **有界帧存储**：`maxFrames` 上限，超出标记 truncated。
- **可观测**：`av_log` 回调接入，CLI 输出 stderr，GUI 走同一条链路。
- **非 ASCII 路径**：GUI 用 UTF-8 把路径传给 FFmpeg（避免中文目录在 GBK 代码页下损坏）。

## 进程隔离、minidump 与 fuzz（P2）

GUI 主进程（`media-analyse.exe`）**不**直接链接 FFmpeg。分析内核被拆成独立的
`analyzer-worker.exe`（只链接 `core`+`infra`+FFmpeg，无 Qt），由主进程通过
`QProcess` 拉起，二者用**行分隔 JSON**在 stdin/stdout 上通信：

- 主进程 → worker：`{"type":"analyze","input":"<url|file>","options":{...}}`（也可发 `{"type":"cancel"}` 取消）。
- worker → 主进程：流式 `{"type":"progress",...}` + 最终 `{"type":"result",<toJson>}`。
- 协议契约集中在 `core/include/ma/core/Serialization.hpp`，CLI `--json`、worker、GUI 三方共用同一 wire format，保证可互解析。

崩溃隔离的好处：**任何 FFmpeg / 解码器崩溃只杀 worker，主窗口保留现场**。
`bridge` 内置看门狗：未收到 `result` 就判定为崩溃，最多**自动重启 worker 一次**，
仍失败则内联诊断并附上 minidump 路径；用户主动取消（`userRequestedStop_`）不会被
误判为崩溃。

### minidump

`infra/CrashHandler.cpp` 安装 `SetUnhandledExceptionFilter` +
`MiniDumpWriteDump`（DbgHelp），并覆盖 `_set_invalid_parameter_handler` /
`_set_purecall_handler` / `std::set_terminate`。崩溃时：

- 在 `<exedir>/crashes/<app>-<ts>.dmp` 写 minidump（`MiniDumpWithDataSegs |
  MiniDumpWithProcessThreadData`）；
- 向 stderr 打印一行 `[CRASHDUMP] <绝对路径>`（worker / CLI / GUI 均如此）。

用 WinDbg / VS 打开 `.dmp` 即可看到崩溃线程与调用栈。`media-analyse.exe`、
`analyzer-worker.exe`、`media-analyse-cli.exe` 启动首行都装了 CrashHandler。

### fuzz 变异回归

`tests/fuzz_worker.py` 对种子媒体做**随机变异 + 截断**（字节翻转 / 插入 / 删除 /
组合、随机前缀截断），逐次拉起 worker（或 CLI），把每次运行分类为：

- `ok`：正常解析；
- `clean_fail`：畸形文件被内核转成结构化诊断（退出 1 / `result.ok=false`）——**这是预期行为，不是崩溃**；
- `crash`：出现 `[CRASHDUMP]` 或 `crashes/` 下新增 `.dmp`，或进程异常退出且无 result；
- `timeout` / `abnormal_exit`：卡死被 kill / 无 result 的异常退出。

目标（计划 P2）：**10 万次变异输入 0 进程退出（无未处理崩溃）**。每次崩溃的输入会被
另存到 `--corpus-dir` 便于单步复现，`.dmp` 默认即时删除以免长时间跑填满磁盘。

```bash
# 对 worker 跑 1 万次变异（Qt 配套工具链的运行时 DLL 需在 PATH 上）
python tests/fuzz_worker.py --exe build/win-mingw-app/analyzer-worker.exe \
                            --seed tests/assets/sample.mp4 --mode worker --iters 10000

# 也可以直接对 CLI 跑（base mingw 工具链）
python tests/fuzz_worker.py --exe build/win-mingw/cli/media-analyse-cli.exe \
                            --seed tests/assets/sample.mp4 --mode cli --iters 10000
```

任何 `crash` / `abnormal_exit` 都会让脚本以非零退出码结束，可直接作为 CI 红线。

## 运行日志与诊断收集（打包版可用）

打包版 GUI 没有控制台，Qt/QML 警告和 FFmpeg 输出原本全部不可见。现在 GUI / worker / CLI 三个进程各写一份滚动 UTF-8 日志到 `logs/`（`<exe目录>/logs` 优先，只读安装自动回退 `%LOCALAPPDATA%\MediaAnalyse\logs`）：

- `media-analyse.log`：启动参数、QML/Qt 警告、控制器与 worker 生命周期（PID / stderr / result / 退出码 / 崩溃重启）；
- `analyzer-worker.log`：请求、FFmpeg `av_log`、阶段耗时、最终诊断；
- `media-analyse-cli.log`：CLI 调用与结果。

每行带毫秒时间戳、级别、进程/线程号、组件名；默认 5 MB × 5 代滚动；URL 凭据（`user:pass@`、`token=`、`key=` 等）自动替换为 `***`。可用 `MA_LOG_DIR` / `MA_LOG_LEVEL`（含 `off`）/ `MA_LOG_MAX_BYTES` / `MA_LOG_MAX_FILES` 调整。

**排查入口**：GUI 顶栏「日志」按钮打开日志目录；双击包内 `collect-logs.cmd` 一键生成 `MediaAnalyse-diagnostics-<时间戳>.zip`（滚动日志 + 最近 3 个 minidump + `info.txt`），直接附到问题单。回归见 `python tests/log_smoke.py`（17 项），详见 `docs/PROJECT_GUIDE.md` §5.4。

## 当前进度

- [x] P0 部分：CMake 工程、依赖锁定（preset 中写死路径）、CLI 与 GUI 双构建
- [x] P1 部分：core/infra 分层、URL 规范化、多流模型、错误诊断、FFmpeg 9.0.2 接入
- [x] P4：PRISM 设计系统 QML 界面（App Bar / Source / Overview / Notice / Streams / Toolbar / Dataview / Status）、`QAbstractTableModel` 虚拟表格 + 文本过滤、矩形选区、内联诊断
- [x] P3 部分：协议自动识别、FFmpeg 高级选项透传（含 RTSP over TCP / 自动重连预设）、探测超时配置
- [x] P2：worker 进程隔离（QProcess + 行分隔 JSON IPC）、minidump（CrashHandler + `[CRASHDUMP]`）、fuzz 变异回归（`tests/fuzz_worker.py`）已落地并通过 1 万次变异 0 崩溃冒烟
- [x] 运行日志与诊断收集：跨进程滚动日志（QML 警告 / FFmpeg 输出 / 凭据脱敏）、GUI「日志」按钮、`collect-logs.cmd` 一键收集；`tests/log_smoke.py` 17/17 通过
- [ ] P2 剩余：样本库与 golden 回归（等实际崩溃样本与流地址清单）
- [x] P3 部分：诊断面板阶段耗时分解（open / find_stream_info / demux，贯穿 CLI human + `--json` + GUI 详情）
- [ ] P3 剩余：`protocol_whitelist` 白名单快捷项、TLS / 自签证书 / 鉴权快捷项

## 注意事项

- `app` 必须用 **Qt 配套的 MinGW 15.1** 编译；用 WinLibs GCC 16 链接 Qt 会因 `__imp___argc` 等运行时符号不兼容而失败。
- windeployqt 默认写出 `Prefix=..` 的 `qt.conf`（假设 bin/ 布局），本项目在构建后自动改写为 `Prefix=.`。
- 修改 `app/qml/Main.qml` 后需重新 build（QML 被编译进资源）。
- 入口使用 `QApplication`（而非 `QGuiApplication`），因为 `QFileDialog` 的原生 Windows 对话框要求 QtWidgets；因此 app/bridge 都链接 `Qt6::Widgets`，windeployqt 会一并部署 `Qt6Widgets.dll`。
