# MediaAnalyse 重构计划

> 调研日期：2026-11-05　｜　对象：`D:\code\MediaAnalyse\MediaAnalyse\MediaAnalyse.exe` 及其发布包
> 方法：实际运行程序并操作 UI、构造样本回放、检查 git 历史与 GitHub 上游、分析 EXE 导入/字符串、直接调用发布包内的 FFmpeg DLL 做对照实验。
>
> **已确认的技术决策（2026-11-05）**：
> 1. 定位为**内部排障工具，不对外商用** → 无闭源/许可证分发压力；
> 2. UI 采用 **Qt Quick / QML**（替代原 Widgets 方案）；
> 3. FFmpeg 升级到**最新稳定版 9.0.2 "Lei"**（2026-09-18，libavutil 61 / libavcodec 63 / libavformat 63）；
> 4. 代码标准 **C++17 起、核心模块可用 C++20 特性**，构建系统 **CMake**。

---

## 一、最重要的结论（先看这条）

**这个仓库里没有任何源代码。** 本地 git 共 15 个提交，全部是 EXE/DLL/图片/语录文本的更新；GitHub 上游 `chenli1379/MediaAnalyse` 的目录树同样只有发布包，没有 `.cpp/.h/.pro/.ui/CMakeLists.txt`。

因此本次不是「改代码」，而是 **基于行为规格重新实现（重写）**。不要投入精力反汇编或修补旧二进制；把旧版当作可执行的"行为基线"，用样本库做回归对照即可。

---

## 二、现状盘点

### 2.1 技术栈与版本

| 项 | 实测结果 |
|---|---|
| UI 框架 | Qt 6 Widgets（Qt6Core/Gui/Widgets/Network/Svg.dll，platforms/qwindows.dll，Qt 自带 29 个语言包） |
| 编译器 | MinGW-w64 / GCC 11.2（EXE 内 `libstdc++-6.dll`、`libwinpthread-1.dll`、gcc 11.2 调试路径与 Itanium ABI 符号） |
| 媒体内核 | FFmpeg **5.1.2 `full_build-www.gyan.dev`**（avcodec-59 / avformat-59 / avutil-57 / avfilter-8 / swscale-6 / swresample-4 / postproc-56） |
| 形态 | 单 EXE（360 KB）+ 约 180 MB DLL 的绿色发布包，无安装包、无源码、无构建脚本 |
| 平台 | 仅 Windows x64 |

EXE 实际导入的 FFmpeg API（据此可还原调用链）：
`avformat_alloc_context / avformat_open_input / avformat_find_stream_info / av_read_frame / avformat_close_input`、
`avcodec_find_decoder / avcodec_alloc_context3 / avcodec_parameters_to_context / avcodec_open2 / avcodec_send_packet / avcodec_receive_frame / avcodec_free_context`、
`av_frame_alloc / av_frame_unref / av_frame_free / av_packet_unref`、
`av_dict_set / av_dict_free`、`av_get_profile_name / av_get_pix_fmt_name / av_get_sample_fmt_name / avcodec_get_name`。
**未导入**：`av_strerror`、`av_log_set_callback`、`avformat_network_init`、`av_seek_frame`、任何 `av_hwdevice_*` / `swscale` / `avfilter` 接口。

内部对象（从符号表还原）：`MainWindow`、`AnalyseThread`(QThread)、`AnalyseWork`、`MediaInfomation`、`MediaFrame` / `VideoFrame` / `AudioFrame`、`Ui_ExtraWindow`（配置）、`Ui_AdmireWindow`（捐助）、全局量 `g_http_headers / g_http_method / g_rtsp_transport`，以及 `Decode(AVCodecContext*, AVFrame*, AVPacket*)`、`interrupt_function(void*)`、`GetVideoFrame(int)`。
信号 `media_data / result / data(MediaInfomation, std::vector<VideoFrame>, std::vector<AudioFrame>)` 说明：**所有帧先攒进两个 vector，分析结束后一次性跨线程回传，再 `QTableWidget::setRowCount + setItem` 全量填表**。

### 2.2 功能与页面 UI（实机操作确认）

- 顶部：媒体类型下拉（**文件 / URL** 二选一）、路径输入框（初始为真实文本 `file://path`，不是占位符）、蓝色「打开」按钮（分析中变「停止」）、灰色「配置」按钮。
- 「文件」模式：点「打开」先弹系统文件对话框，选定后回填为 `D:/.../xxx.mp4` 形式并开始分析；「URL」模式：直接使用输入框内容。
- 中部三个分组框：
  - **媒体信息**：音频时长、视频时长、封装格式
  - **视频信息**：类型(profile)、等级(level)、帧率、分辨率、时间基、编码格式
  - **音频信息**：类型(profile)、声道数、采样数、采样率、时间基、编码格式
- 下部：`TabWidget`（**视频 / 音频**）+ `QTableWidget`。视频列：`dts, dts-time, dts-delta, pts, pts-time, duration, duration-time, size, width, height, pix_fmt, pict_type`；音频列含 `sample_fmt, samples, channels`。
- 底部：随机「程序员语录」（来自 `signature.txt`）+ 「捐助」链接（弹出 `AdmireWindow`，含微信收款码与联系邮箱）。
- 「配置」独立窗口（标题「配置信息」，434×509）：**RTSP 配置** `rtsp_transport` = udp/tcp；**HTTP 配置** `method` = get/post/put；**header 配置** 固定 3 行 key/value；**自定义配置** 固定 2 行 key/value。**没有确定/取消/保存按钮**，关闭窗口时由 `ExtraWindow::GetExtras()` 读取保存。
- 失败时弹出 `QMessageBox`「告警」：`媒体解析失败或者不支持媒体，请检查媒体后重试。`，按钮为英文 OK（未汉化）。

### 2.3 关键参数与已知修复历史

- EXE 中 `10000000` 与 `probesize`、`20000000` 与 `analyzeduration` 相邻出现，判断为**硬编码探测上限**（10 MB / 20 s），配置界面未暴露（仅"自定义配置"可间接覆盖）。
- 用 `avformat_alloc_context` + `interrupt_function` 实现中断；git 提交 `1c59432` 明确写着「**增加中断函数，媒体流 6 秒没拉到就终止**」。
- 历史提交反映的既有缺陷：`00770ac`「修复**只有一路流奔溃**问题」、`6ef7254`「修复**音频 ts 解析 bug**、修复分段显示 bug、新增解析失败告警」、`8adad1c`「添加停止功能」。
- 使用 `SetUnhandledExceptionFilter`，但没有可读日志、没有 dump、没有 `av_strerror` 错误码转换。

### 2.4 实测矩阵（本次实机回放）

| # | 输入 | 结果 |
|---|---|---|
| 1 | 正常 MP4（h264 320×176 25fps + aac 48kHz 2ch，文件模式） | ✅ 完整解析：时长 0:0:10.5 / 0:0:9.880、`mov,mp4,m4a,3gp`、Main/12、90000 时间基、LC/2ch/1024/48000，帧表有数据 |
| 2 | 纯音频 WAV（单流，文件模式） | ✅ 能解析，但视频区全空、默认停留在空的「视频」Tab、采样数显示 0 |
| 3 | 1 MB 纯随机 `.mp4` | ⚠️ 统一告警，未崩溃 |
| 4 | 截断到 200 KB 的 MP4 | ⚠️ 统一告警，未崩溃 |
| 5 | 头部完整、中部 200 KB 数据打乱的 MP4 | ✅ 静默解析出结果，**没有任何数据完整性提示** |
| 6 | `file:///C:/.../sample.mp4`、`file:///.../audio.wav`（URL 模式） | ❌ 统一告警"不支持" |
| 7 | `https://www.w3schools.com/html/mov_bbb.mp4`（URL 模式） | ❌ 统一告警"不支持"；**失败后信息区仍显示上一次文件的数据**，表格却被清空 |

对照实验（直接调用**同一个发布包里的** `avformat-59.dll`）：

| URL | `avformat_open_input` | `avformat_find_stream_info` |
|---|---|---|
| `C:/.../sample.mp4` | 0 OK | 0 OK，0.00 s |
| `file:C:/.../sample.mp4` | 0 OK | 0 OK |
| `file:///C:/.../sample.mp4` | **-2 No such file or directory** | — |
| `https://www.w3schools.com/html/mov_bbb.mp4` | **0 OK** | **0 OK，约 1.6 s**（不调用 `avformat_network_init` 也成功） |

> 两个决定性事实：
> 1. Windows 下 FFmpeg 5.1 不接受 `file:///C:/...` 三斜杠写法，而程序把用户输入**原样透传**，且默认输入框还预填了真实文本 `file://path`（直接粘贴会拼成 `file://pathhttps://...`）。
> 2. **同一个 HTTPS 地址，同一套 FFmpeg DLL，1.6 秒即可完成打开与探测；程序却在 6 秒内报"不支持"** → 这不是 FFmpeg 缺能力、也不是编码格式问题，而是程序侧的 URL 处理 / 参数传递 / 中断回调逻辑有缺陷。程序日志经 stdout/stderr 重定向捕获为空（应为 `qDebug`/OutputDebugString 输出），无法读出具体错误码，重构时必须先把日志做对。

---

## 三、三个痛点的根因

### A. 编码格式 / 分辨率支持有限

1. **用"完整解码"换帧信息**：`avcodec_send_packet/receive_frame` 对**每个包**都做解码，只为拿 width/height/pix_fmt/pict_type。找不到解码器、硬件帧、非常规像素格式（`p010/yuv420p10le/cuda` 等）时易走进失败分支，而 FFprobe 的纯元数据路径本可正常出结果。
2. **FFmpeg 锁死在 5.1.2（2022）**：缺少后续对新编码（AV1 生态、H.266/VVC、新 HEVC 扩展）、新封装与协议修复的支持。
3. **硬编码 `probesize=10 MB` / `analyzeduration=20 s`**：高码率 4K/8K、长 GOP、稀疏流探测不足，容易"探不到流"而被判不支持。
4. **字段模型写死**：三块信息区 + 两个 Tab 假设"一路视频 + 一路音频"；多路流、纯音频/纯视频、字幕/数据流、旋转/SAR、HDR/10bit、多节目 TS 没有建模。实测帧表前两行 width/height 为空却出现在「视频」Tab，说明包与流的归属处理混乱。
5. **全量内存 + 全量控件**：所有帧存 `std::vector` 再一次性灌 `QTableWidget`，长视频/高分辨率下内存与 UI 双爆，`QTableWidgetItem` 数量是帧数 × 列数。

### B. 异常媒体文件崩溃

1. 历史提交直接证实「只有一路流」「音频 ts」曾是崩溃点，说明代码按固定下标/固定流顺序访问 `streams[]`，修的是具体样本而非结构性防护。
2. `av_get_profile_name` / `av_get_pix_fmt_name` / `avcodec_find_decoder` 等**可能返回 NULL**，字符串构造与格式化没有统一保护。
3. 解码失败、`EAGAIN/EOF`、部分损坏、零帧、超大帧数、负数 dts 等边界分支不完整（EXE 内仅留下 `Error sending a packet for decoding` / `Error during decoding` 两句话）。
4. 跨线程信号携带大 vector 拷贝，`stop()` / 重复点击 / `QThread::quit()+wait()` 与分析线程生命周期存在竞态。
5. 失败路径不清理状态（实测 #7 旧数据残留），用户会基于错误数据做判断。
6. 本次用随机垃圾、截断、中部损坏样本**未复现崩溃**，说明用户现场崩溃需要特定条件。缺日志、缺 minidump、缺样本，是"查不动"的根本原因 —— 重构必须先补可观测性，再谈修复。

### C. 媒体流地址显示不支持

1. **URL 未规范化**：`file:///C:/...` 实测返回 -2；预填文本拼接问题；无 trim、无 scheme 白名单、无中文/空格/转义处理、无重定向跟踪。
2. **参数与超时策略僵化**：6 秒一刀切中断、10 MB/20 s 探测上限硬编码；缺 `protocol_whitelist`（HLS/DASH 嵌套协议最常见失败原因）、`reconnect`、`rw_timeout`、`stimeout`、UA/Cookie/Referer/鉴权、TLS 选项。
3. **配置面过窄**：只有 udp/tcp、get/post/put、3 行 header、2 行自定义；无预设、无校验、无确定/取消、无导入导出、无按协议分组的高级项。
4. **错误信息不可用**：所有失败统一成一句"解析失败或者不支持媒体"，没有 `av_strerror` 错误码、失败阶段（open/find_stream_info/read_frame/decode）、HTTP 状态码、耗时、URL 脱敏回显 —— 用户无法自助定位，开发者也无法复现。

---

## 四、重构目标与原则

**目标**：任意 FFmpeg 可识别的格式/分辨率都能给出结果；任何异常输入都不导致进程退出；任何流地址都能被正确尝试并给出可定位的诊断；长视频 UI 不卡顿。

**原则**：
- **元数据优先，解码按需**：默认只 demux 出包级 + 流级信息；`width/height/pix_fmt/pict_type` 等需要解码才有的信息改为"按需/抽样/后台"生成。
- **内核与 UI 解耦**：核心为无 Qt 依赖的纯 C++ 库，可被 CLI、测试、GUI 复用。
- **错误即数据**：每次 FFmpeg 调用统一封装为 `{code, av_strerror 文本, 阶段, 上下文}`，UI 展示结构化诊断而非一句话。
- **流式与有界**：帧数据进入有上限的列式/环形存储，表格用 Model/View 虚拟滚动，不把全部帧一次性塞进控件。
- **可观测、可回放**：日志 + minidump + 样本回放是所有"崩溃/不支持"问题的前置条件。
- **配置即数据**：协议参数版本化、可导入导出、有预设与校验。
- **行为对齐**：无源码，先用样本库固化旧版输出作为 golden baseline，新旧双跑对比后再切换。

---

## 五、目标架构

```
app/            Qt Quick / QML 前端（Qt 6.8 LTS 及以上，推荐 6.9/6.10）
  Main.qml          ApplicationWindow + 状态机（空闲→分析中→成功/失败/取消）
  SourceBar.qml     文件/URL 切换、路径输入（占位符而非预填文本）、打开/停止
  InfoPanels.qml    媒体/视频/音频信息卡（按实际流动态生成，Repeater 而非写死三块）
  StreamTabs.qml    TabBar + TableView（QML），列由流类型决定
  FrameTableModel   C++ QAbstractTableModel 暴露给 QML TableView（虚拟滚动、增量加载）
  ConfigDialog.qml  按协议分组的配置页：RTSP/HTTP/HLS/RTMP/超时/白名单/重连/TLS，header 可增删
  Diagnostics.qml   诊断面板：各阶段耗时、错误码、av_strerror 文本、脱敏 URL、一键复制
  主题             单一 QML 单例主题（亮/暗色，Material/Basic 风格），高 DPI 天然支持
bridge/         薄适配层（唯一允许 core ↔ Qt 交互的地方）
  SignalsBridge     把 core 回调转 Qt 信号；core 类型 → QVariantMap/QAbstractTableModel
core/           纯 C++20，不依赖 Qt，可被 CLI、测试、GUI 复用
  MediaSource       URL 规范化、scheme/协议能力检测、本地/网络判别
  ProbeSession      open → find_stream_info → 结构化错误
  DemuxLoop         av_read_frame，可取消、可限流
  FrameInspector    元数据优先；可选解码路径（抽样/按需）
  StreamModel       任意路数流（video/audio/subtitle/data/attachment）
  FrameStore        列式存储 + 环形缓冲 + 内存上限
  Result / Error    错误码 + av_strerror + 阶段 + 上下文
infra/          纯 C++，不依赖 Qt
  FFmpeg RAII       AVFormatContext/AVCodecContext/AVPacket/AVFrame/AVDictionary（C++20 Concepts 约束 RAII 模板）
  Logging           av_log_set_callback → 分级日志（文件 + UI）
  Interrupt         分阶段超时（连接/首包/无数据），取代 6 秒一刀切
  CrashHandler      SEH minidump + 线程异常边界 + 样本回放入口
test/           样本库 / golden 回归 / fuzz / 本地流媒体 mock server / 性能基准
build/          CMake ≥ 3.28 + vcpkg（ffmpeg、Qt）+ 预设 presets；CI(Windows) + windeployqt 打包
```

**技术选型落实细节**：
- **C++ 标准**：全局 `CMAKE_CXX_STANDARD 20`（满足"C++17 以上"，核心库直接用 C++20 的 `concepts / spans / std::format`；如需兼容旧工具链可降为 17，仅禁用 `std::format`）。
- **编译器**：Windows 上首选 **MSVC 2022**（与 Qt 官方预编译包、vcpkg 二进制缓存匹配度最好）；如团队沿用 MinGW 也可，但 FFmpeg 需自行编译或选对应工具链的预编译包。
- **QML 关键点**：帧表必须用 `TableView + QAbstractTableModel`（不要在 QML 里 `ListModel` 逐行 append，百万行会卡死）；大结果集走 C++ 模型分页/懒加载；core 保持无 Qt，QML 只消费 bridge 暴露的模型与信号。
- **worker 进程隔离**仍然保留：分析内核放独立子进程（`analyzer-worker.exe`），主 UI 通过 IPC 通信。任何 FFmpeg/解码器崩溃只杀 worker，主窗口保留现场、自动重启 worker 并生成 dump —— 这是"永不崩溃"最省力也最可靠的做法。

---

## 六、分阶段计划与验收

| 阶段 | 主要任务 | 交付物 | 验收标准 |
|---|---|---|---|
| **P0 基线**（约 1 周） | 建立 CMake 工程（presets + vcpkg 锁定 FFmpeg 9.0.2、Qt 6.8 LTS+、MSVC 2022、C++20）与 CI；归档旧版 EXE/DLL/哈希/`Images/ui.png`；建样本库（正常/单流/损坏/4K/HDR/各类流）；把旧版输出固化为 golden | 《现状行为规格》+ 样本库 + CI 流水线 | 旧版可在 CI 里无人值守跑完样本库并生成对照报告 |
| **P1 内核重写**（2–3 周） | FFmpeg RAII + 统一错误 + 日志回调 + 分阶段超时；URL 规范化；多路流模型；元数据优先路径；升级 **FFmpeg 9.0.2** 并输出**格式/编码/像素格式/分辨率/协议能力矩阵** | `core/` + `infra/` + CLI 版 analyzer | 样本库 100% 得到"结果或带错误码的明确失败"；4K/8K、10bit、AV1/VVC、纯音频、多路流全部通过；**0 崩溃** |
| **P2 健壮性**（1–2 周） | 解码按需与失败降级；NULL/越界/异常全防护；worker 进程隔离 + watchdog；minidump + 日志 + 样本回放；fuzz（随机变异 + 截断） | crash-handler + fuzz 报告 | 10 万次变异输入 0 进程退出；任一失败都能给出错误码、阶段、脱敏 URL；旧数据不再残留 |
| **P3 流媒体**（1–2 周） | 协议预设（HTTP/HTTPS/HLS/DASH/RTSP/RTMP/SRT/UDP…）、`protocol_whitelist`、reconnect、分层超时、TLS/证书、header/method/cookie/鉴权；诊断面板显示各阶段耗时与 HTTP 状态 | 配置中心 + 诊断面板 | 对照实验中失败的 HTTPS 地址成功解析；HLS/RTSP/RTMP 样例通过；人为故障能定位到具体阶段 |
| **P4 QML UI**（约 2 周） | QML 界面（SourceBar/InfoPanels/StreamTabs/ConfigDialog/Diagnostics）；`FrameTableModel(QAbstractTableModel)` 接 QML `TableView` 虚拟滚动；增量加载、搜索/过滤/导出 CSV/JSON；进度与取消；失败清空旧数据；亮/暗主题 | 新 QML UI | 100 万行帧表内存受控、首屏 < 2 s、滚动不掉帧；信息区与 Tab 由实际流动态生成（Repeater）；高 DPI 与暗色正常 |
| **P5 发布与迁移**（约 1 周） | 版本号/变更日志、绿色发布包（windeployqt + FFmpeg DLL）、内部使用说明、双跑对照报告、旧配置迁移 | v1.0 内部发布包 | 新旧版在样本库上结果一致或差异有解释；同事可从旧配置平滑迁移 |

> 内部工具定位下 P5 大幅简化：无需安装包/自动升级/许可证审计；gyan.dev full build（GPL）直接可用，无需改 LGPL 方案。

**若资源有限，优先做 P0 + P1 + P2**：内核重写 + FFmpeg 升级 + URL 规范化 + 崩溃隔离，就能一次性覆盖你提出的三个痛点中的绝大部分；P3 可紧随其后。

---

## 七、关键设计决策清单

1. **FFmpeg 版本（已定）**：升级到 **9.0.2 "Lei"**（当前最新稳定版，2026-09-18；libavcodec 63 / libavformat 63）。从旧版 5.1.2（avcodec 59）跨 4 个大版本，`infra/` 封装必须吸收这些破坏性变更：`AVCodecContext.ch_layout` 强制使用（旧 `channels/channel_layout` 已删除）、`AVCodec/AVFormatContext` 大量 const 化、废弃 API 全部移除（`av_init_packet`、`avcodec_register_all` 等）、`AVFrame` 时间戳/时长语义调整。升级只改 `infra/` 一层。
2. **分辨率/像素格式**：全部以 `AVFrame/AVCodecParameters` 的动态值呈现，不设上限、不做白名单；`pix_fmt` 名称为空时显示枚举数值而非空指针；补充 `color_range/primaries/trc/space`、SAR/DAR、旋转、HDR 元数据。
3. **帧表**：默认只分析包级（`dts/pts/duration/size/flags/keyframe`），需要 `width/height/pix_fmt/pict_type` 时按需解码；`duration` 为 0 时按时间基推算并标注来源。
4. **探测参数**：`probesize/analyzeduration` 改为可配置 + 按协议自适应（本地大文件加大，网络流减小并配合 reconnect）；不再硬编码。
5. **超时**：连接超时 / 首包超时 / 无数据超时三段独立可配，替代"6 秒没拉到就终止"。
6. **错误展示**：`av_strerror` + 阶段 + 建议动作（例如"该 file:/// 写法不被支持，已自动转换为 file:C:/..."），并提供一键复制诊断信息。
7. **状态一致性**：任何新分析开始前清空三块信息区与两张表；失败时保留"上次成功"但必须显式标注为历史结果。
8. **UI 技术（已定）**：Qt Quick/QML。QML 只做视图与交互，所有媒体数据经 `bridge/` 以 Qt 模型/信号进入 QML；帧表性能关键路径在 C++ 模型内，QML 侧禁止逐行填充。
9. **语言与构建（已定）**：C++20（不低于 C++17）、CMake ≥ 3.28 + presets + vcpkg 依赖锁定；core/infra 不依赖 Qt，保证 CLI 与单测可直接链接。

---

## 八、风险与合规

- **许可证（已按"内部使用"定案）**：工具仅在内部排障使用、不对外分发 → GPL 版 FFmpeg（gyan.dev full build）与 LGPL 动态链接 Qt 均可直接使用，无需改造。唯一注意点：若将来某个内部同事把带 x264/x265 的 DLL 包传到公网或集成进对外产品，就触发 GPL 义务 —— 在发布包里放一份 NOTICE 说明"仅限内部使用"即可。
- **FFmpeg 9.0 升级风险**：跨 4 个大版本，除 API 删除外，部分 demuxer/decoder 的探测结果会变（帧率、时长、profile 命名）；golden 基线允许"有解释的差异"，不要硬比对字符串。vcpkg/CI 一开始就锁 `ffmpeg==9.0.2`，后续升级走显式变更。
- **QML 风险**：帧表性能依赖 `TableView + QAbstractTableModel` 的正确用法；QML 调试体验弱于 Widgets，P4 预留 `QT_QML_DEBUG` 与模型行数断言；Windows 上 Qt Quick 渲染（RHI/D3D11）在部分老显卡/远程桌面场景可能需要回退软件渲染（`QT_QUICK_BACKEND=software`），作为排障工具要预留这个开关。
- **无源码**：不要指望还原 100% 旧行为；用 golden 样本定义"必须一致"的核心字段，其余按新设计改进并在变更日志说明。
- **网络测试不可重复**：CI 里用本地 mock（本地 HLS 切片、本地 RTSP/RTMP 服务、受限 HTTPS 服务）覆盖协议分支，避免依赖公网。
- **崩溃现场缺失**：先上线日志/dump/样本回灌机制，再关闭"崩溃"类问题；同事样本需征询同意后再收集。
- **编译器/工具链**：首选 MSVC 2022（Qt 与 vcpkg 预编译匹配最好）；若坚持 MinGW，FFmpeg 9.0 需要用兼容的 MinGW 构建或自行编译，避免混用 CRT。

---

## 九、需要你补充的输入

1. 2–5 个**实际会崩溃**的媒体文件（或最小复现样本）与操作步骤；有 Windows 事件查看器的错误模块名更好。
2. 一份**显示"不支持"的流地址清单**（可脱敏），注明协议、是否需要鉴权/内网/自签证书、以及用 VLC/ffprobe 能否打开。
3. ~~目标用户场景~~ ✅ 已确认：内部排障工具，不商用。
4. ~~UI 技术偏好~~ ✅ 已确认：Qt Quick / QML。
5. 是否保留「捐助」页、随机语录等非功能性元素 —— 内部工具建议**直接裁剪**，如无异议按裁剪执行。

## 十、已定决策摘要

| 决策项 | 结论 | 影响 |
|---|---|---|
| 定位 | 内部排障工具，不商用 | 无许可证改造压力；P5 免安装包/审计；捐助页建议裁剪 |
| UI | Qt Quick / QML | 新增 `bridge/` 适配层；帧表用 QAbstractTableModel + TableView；预留软件渲染回退 |
| FFmpeg | 9.0.2 "Lei"（最新稳定，2026-09-18） | 跨 4 个大版本升级，`infra/` 吸收 avcodec 59→63 破坏性变更；vcpkg 锁版本 |
| 语言/构建 | C++20（≥C++17）、CMake ≥3.28 + vcpkg + MSVC 2022 | core/infra 无 Qt 依赖；CI 从 P0 起锁定全部依赖版本 |

---

## 十一、实施进展（2026-11-05）

代码位于 `D:\code\MediaAnalyse\media-analyse`，工程已可编译运行。

**工具链（全部绿色便携，无 UAC/无安装程序）**

| 组件 | 版本 | 路径 |
|---|---|---|
| GCC + CMake + Ninja | 16.2.0 / 4.4.4 / 1.13.2 | `D:\CodeTools\mingw64` |
| Qt | 6.10.3 (mingw_64) | `D:\CodeTools\Qt\6.10.3\mingw_64` |
| Qt 配套 MinGW | 15.1.0 | `D:\CodeTools\Qt\Tools\mingw1510_64` |
| FFmpeg | 9.0.2 (avcodec 63.1.102) | `D:\CodeTools\ffmpeg-n9.0-latest-win64-gpl-shared-9.0` |

**已完成**

- `core/`：`Result`/`Diagnostic`（错误即数据）、`UrlNormalizer`（`file:///C:/` → `file:C:/` 等 7 类修正）、`MediaInfo` 多流 DTO、`Analyzer` 接口。
- `infra/`：FFmpeg RAII、`av_log` 桥接、`InterruptGuard` 分阶段超时 + 协作取消、`ProbeAnalyzer`（open → find_stream_info → demux，可选解码，帧数有界）。
- `cli/`：`media-analyse-cli`，支持 `--json/--frames/--decode/--opt`，静态链接单文件。
- `bridge/` + `app/`：`AnalysisController`（工作线程 + 取消 + 诊断信号）、`FrameTableModel`（10 列 `QAbstractTableModel`）、QML 深色界面（源栏 / 流卡片 / 双 Tab 帧表 / 配置弹窗 / 失败诊断弹窗），支持命令行传参直接分析。

**冒烟测试结果**

| 场景 | 旧版 | 新版 |
|---|---|---|
| 正常 MP4（4 路流：video+aac×2+data） | 仅两路 | 4 路全部列出，帧表 1190 行 |
| `file:///C:/...` URL | ❌ 不支持 | ✅ 自动转 `file:C:/...` 并成功 |
| 真实 HTTPS 地址 | ❌ 不支持 | ✅ 成功解析 |
| 纯音频 WAV | 可解析但停在空视频 Tab | ✅ 单流正确展示 |
| 随机垃圾 mp4 | 一句「解析失败」 | ✅ 阶段 `open` + 错误码 `-1094995529` + 原文 |
| 不存在的路径 | 一句「解析失败」 | ✅ `file-not-found` + 建议 |

**下一步**：worker 进程隔离 + minidump（P2）→ 协议配置中心与诊断面板（P3）→ 样本库与 golden 回归（等实际崩溃样本、不支持地址清单）。
