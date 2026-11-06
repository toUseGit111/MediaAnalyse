# MediaAnalyse 重构版 v1.0 —— 内部排障工具发布说明

> 构建日期：2026-11-06　｜　状态：v1.0 内部可用（P0/P1/P4 完成；P2 进程隔离 + minidump + fuzz 完成；P2 样本库 / P3 剩余待补）
> 工程说明（目录 / 编译 / 打包 / 使用）：[`docs/PROJECT_GUIDE.md`](docs/PROJECT_GUIDE.md)

## 1. 定位与技术栈

- **定位**：内部排障工具，**不对外商用**。原仓库无源码，本版基于旧版行为规格从头重写。
- **UI**：Qt 6.10.3 Quick / QML（深色 PRISM 风格）
- **媒体内核**：FFmpeg 9.0.2（avcodec 63 / avformat 63 / avutil 61）
- **语言 / 构建**：C++20 + CMake（presets：GUI `win-mingw-app`、CLI `win-mingw`）

## 2. 相比旧版解决的核心问题

| 痛点 | 旧版 | 重构版 |
|---|---|---|
| 编码/分辨率支持有限 | 用整包解码换帧信息，非常规像素格式易失败 | 元数据优先 + 按需解码；不限制格式/分辨率 |
| 流地址「不支持」 | HTTPS/RTSP/RTMP 一律告警 | URL 自动规范化，HTTPS/RTSP/RTMP/HLS/HTTP-FLV 直接解析 |
| 异常文件崩溃 | 缺日志、缺 dump | 结构化诊断（错误码+阶段+av_strerror 原文+建议），不崩溃 |
| 流假设 | 写死「一路视频+一路音频」 | 任意路数流（video/audio/subtitle/data）动态建模 |
| 大文件卡顿 | 全量塞 QTableWidget | `QAbstractTableModel` + `TableView` 虚拟滚动 |

## 3. 用法

### 3.1 CLI

```
media-analyse-cli [options] <url|file>
  --json --frames=N           输出 JSON，含前 N 行帧（默认全部）
  --decode                    补充帧级 dims/format/ch（按每条流独立预算）
  --connect-timeout S         连接超时（秒）
  --stall-timeout S           无数据超时（秒）
  --probe-timeout S           探测（find_stream_info）超时（秒）
  --max-frames N              最多读取包数
  --opt K=V                   透传 FFmpeg 高级选项（如 rtsp_transport=tcp）
```

示例：

```
media-analyse-cli --json --frames=20 tests/assets/sample.mp4
media-analyse-cli --opt rtsp_transport=tcp rtsp://127.0.0.1:8554/live
```

### 3.2 GUI

- **文件模式**：点「打开」弹原生文件对话框，选完自动分析；也可直接粘贴路径回车。
- **网络流模式**：输入地址，协议自动识别（AUTO 徽标显示如 `AUTO · RTSP`），实时流显示 `LIVE`。
- **帧表**：矩形选区（蓝底白字，对齐旧版 QTableWidget 体验）、`Ctrl+C` 或「复制选区」导出 TSV、滚轮滚动、`#` 列点选整行、过滤框按内容筛选。
- **停止解析**：中性操作——**不弹错误框**，保留已解析的部分结果并停止后续读取；若停止时尚未读到任何包（慢连接/探测超时），则保留上一次结果。
- **配置弹窗**：连接/无数据/探测超时、最大帧数、最大解码帧数、RTSP over TCP、自动重连、以及 `K=V` 高级 FFmpeg 选项透传。

## 4. 帧表列（按流类型自适应，各 13 列）

- **视频 Tab**：`# pkt# stream dts dts-time dts-delta pts pts-time duration size type dims format`
- **音频 Tab**：`# pkt# stream dts dts-time dts-delta pts pts-time duration size samples format ch`

列释义：

- `type`：由码流 parser 解析的 `I / P / B / S`（只解析帧头，不解码像素，速度接近纯 demux）。
- `samples`：音频 packet 按采样率换算的采样数（AAC 通常 1024）。
- `dims`（视频）/ `format` / `ch`（音频）：**解码专属列**，需勾选「解码」才显示具体值；超出每流解码预算时，仅前若干帧显示，不丢行。

## 5. 验收结果

- CLI 回归 `tests/smoke_cli_json.py`：**14/14 通过**（JSON 合法性、I/P/B、samples、解码纯附加、按流预算、提示语等）。
- GUI 实机：文件模式、网络流模式、矩形选区/复制、过滤、滚轮、视频/音频 Tab 列裁剪、解码补充、配置弹窗、停止部分结果均无 QML 报错。
- 本地 MediaMTX + FFmpeg 推流：RTSP / RTMP / HLS 均可解析并停止于部分结果（无弹窗）。
- **进程隔离**：GUI 经 `QProcess` 拉起 `analyzer-worker.exe`，控制器头测 `tests/controller_test.cpp` 验证完整链路 `controller → QProcess → worker → JSON → fromJson → analysisSucceeded` 通过。
- **worker 生命周期**：worker 输出 `result` 后立即退出（不再阻塞在 stdin 读线程），`tests/gui_integration_test.py` 以「10 s 内必须退出」为硬断言；worker 与 FFmpeg/MinGW DLL 由 `ma_deploy_runtime` ALL 目标在每次构建时对账拷贝，避免只改 worker 时 GUI 目录残留旧副本。
- **工程结构**：源码目录已从 `media-analyse/` 上移到仓库根，构建产物统一在 `build/<preset>/`；`python scripts/package.py` 产出可分发的 `dist/MediaAnalyse-v1.0/`。
- **阶段耗时诊断（P3）**：`StageTimings` 记录 open / find_stream_info / demux / total；CLI `timings` 行、`--json` 的 `timings` 对象、GUI「详情」toast 均展示，失败时保留已发生阶段（未到达为 `null`/`—`）。`ser_test` 覆盖其序列化往返（含 `-1 → null → -1`）。
- **fuzz 变异回归** `tests/fuzz_worker.py`：1 万次随机变异 + 截断输入喂给 worker，**0 未处理崩溃**（crash=0 / timeout=0 / abnormal_exit=0）；畸形输入全部被内核转为结构化诊断（`clean_fail`），不崩溃进程。

## 6. 已知限制与后续待办

- **P2 进程隔离 + minidump + fuzz 已完成**：分析内核拆为独立 `analyzer-worker.exe`，崩溃只杀 worker 并写 `<exedir>/crashes/<app>-<ts>.dmp`（stderr 打 `[CRASHDUMP]` 行）；看门狗最多自动重启一次；`tests/fuzz_worker.py` 跑通 1 万次变异 0 崩溃。**样本库 / golden 回归仍待补**（需实际崩溃样本与流地址清单）。
- **P3 流媒体剩余**：`protocol_whitelist` 白名单快捷项、TLS / 自签证书 / 鉴权快捷项（阶段耗时分解已完成：open / find_stream_info / demux 贯穿 CLI 与 GUI）。
- **P5 发布**：绿色包已可脚本化产出——`python scripts/package.py` 从构建树挑出 `media-analyse.exe` + `analyzer-worker.exe` + Qt/FFmpeg DLL + 插件/QML 模块，落到 `dist/MediaAnalyse-v1.0/`（不含测试 exe 与日志）；版本号与变更日志仍为人工维护。

## 7. 故障排查

- 链接报 `Permission denied`：旧进程占用 exe，先 `taskkill /F /IM media-analyse.exe` 再构建。
- 远程桌面 / 老显卡渲染异常：设 `QT_QUICK_BACKEND=software` 回退软件渲染。
- **分析内核崩溃**：若某次分析失败且 GUI 提示含 `crashes/...dmp`，说明 `analyzer-worker.exe` 触发了 minidump（FFmpeg/解码器崩溃被隔离，主窗口仍在）。用 WinDbg / Visual Studio 打开该 `.dmp` 看崩溃线程与调用栈；同时复现输入可交给 `tests/fuzz_worker.py --corpus-dir` 做单步回归。worker 默认最多自动重启一次，仍失败则内联报错并附 dmp 路径。
