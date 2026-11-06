// ma::bridge - implementation of FrameTableModel and AnalysisController.
#include "ma/bridge/Bridge.hpp"

#include <algorithm>

#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

#include "ma/infra/ProbeAnalyzer.hpp"

namespace ma::bridge {

// ---------------- FrameTableModel ----------------

namespace {

QString fmtInt64(int64_t v) {
    return v == INT64_MIN ? QStringLiteral("-") : QString::number(v);
}

QString fmtOpt(const std::optional<double>& v, int prec = 6) {
    return v ? QString::number(*v, 'f', prec) : QStringLiteral("-");
}

// Visible -> internal column mapping. Both tabs expose 13 columns; the video
// tab drops samples(11)/ch(14) while the audio tab drops type(10)/dims(12).
const int kV[13] = {
    FrameTableModel::ColIndex, FrameTableModel::ColSeq, FrameTableModel::ColStream,
    FrameTableModel::ColDts, FrameTableModel::ColDtsTime, FrameTableModel::ColDtsDelta,
    FrameTableModel::ColPts, FrameTableModel::ColPtsTime, FrameTableModel::ColDuration,
    FrameTableModel::ColSize, FrameTableModel::ColType, FrameTableModel::ColDims,
    FrameTableModel::ColFormat};
const int kA[13] = {
    FrameTableModel::ColIndex, FrameTableModel::ColSeq, FrameTableModel::ColStream,
    FrameTableModel::ColDts, FrameTableModel::ColDtsTime, FrameTableModel::ColDtsDelta,
    FrameTableModel::ColPts, FrameTableModel::ColPtsTime, FrameTableModel::ColDuration,
    FrameTableModel::ColSize, FrameTableModel::ColSamples, FrameTableModel::ColFormat,
    FrameTableModel::ColChannels};
const char* HV[13] = {"#", "pkt#", "stream", "dts", "dts-time", "dts-delta", "pts",
                      "pts-time", "duration", "size", "type", "dims", "format"};
const char* HA[13] = {"#", "pkt#", "stream", "dts", "dts-time", "dts-delta", "pts",
                      "pts-time", "duration", "size", "samples", "format", "ch"};

} // namespace

QString FrameTableModel::cellText(const ma::FrameRecord& f, int column,
                                  int visibleRow) {
    switch (column) {
    case ColIndex: return QString::number(visibleRow + 1);
    case ColSeq: return QString::number(f.seq);
    case ColStream: return QString::number(f.streamIndex);
    case ColDts: return fmtInt64(f.dts);
    case ColDtsTime: return fmtOpt(f.dtsTime, 3);
    case ColDtsDelta: return fmtOpt(f.dtsDelta, 3);
    case ColPts: return fmtInt64(f.pts);
    case ColPtsTime: return fmtOpt(f.ptsTime, 3);
    case ColDuration: return QString::number(f.duration);
    case ColSize: return QString::number(f.size);
    case ColType:
        // Audio/data rows have no picture type. Video key frames are already
        // labelled "I" in packetToRecord, so no "K" fallback is needed here.
        return f.pictType ? QString::fromStdString(*f.pictType)
                          : QStringLiteral("-");
    case ColSamples:
        return f.samples ? QString::number(*f.samples) : QStringLiteral("-");
    case ColDims:
        return (f.width && f.height)
                   ? QStringLiteral("%1x%2").arg(*f.width).arg(*f.height)
                   : QStringLiteral("-");
    case ColFormat:
        if (f.pixFmt) return QString::fromStdString(*f.pixFmt);
        if (f.sampleFmt) return QString::fromStdString(*f.sampleFmt);
        return QStringLiteral("-");
    case ColChannels:
        return f.channels ? QString::number(*f.channels) : QStringLiteral("-");
    default: return {};
    }
}

void FrameTableModel::rebuildMap() {
    rowMap_.clear();
    if (!frames_ || filter_.isEmpty()) return;
    const QString needle = filter_.toCaseFolded();
    rowMap_.reserve(frames_->size());
    for (size_t i = 0; i < frames_->size(); ++i) {
        if (rowText(static_cast<int>(i)).toCaseFolded().contains(needle))
            rowMap_.push_back(static_cast<int>(i));
    }
}

void FrameTableModel::reset(const std::vector<ma::FrameRecord>* frames) {
    beginResetModel();
    frames_ = frames;
    filter_.clear();      // a new result starts unfiltered
    rebuildMap();
    endResetModel();
}

void FrameTableModel::setFilter(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed == filter_) return;
    beginResetModel();
    filter_ = trimmed;
    rebuildMap();
    endResetModel();
}

int FrameTableModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !frames_) return 0;
    if (filter_.isEmpty()) return static_cast<int>(frames_->size());
    return static_cast<int>(rowMap_.size());
}

int FrameTableModel::sourceRow(int visibleRow) const {
    if (!frames_ || visibleRow < 0) return -1;
    if (filter_.isEmpty()) {
        return visibleRow < static_cast<int>(frames_->size()) ? visibleRow : -1;
    }
    return visibleRow < static_cast<int>(rowMap_.size()) ? rowMap_[visibleRow] : -1;
}

QString FrameTableModel::rowText(int sourceRow) const {
    if (!frames_ || sourceRow < 0 || sourceRow >= static_cast<int>(frames_->size()))
        return {};
    const ma::FrameRecord& f = (*frames_)[sourceRow];
    QStringList cells;
    cells.reserve(ColumnCount);
    for (int c = 0; c < ColumnCount; ++c)
        cells << cellText(f, c, sourceRow);
    return cells.join(QLatin1Char(' '));
}

int FrameTableModel::visibleToInternal(bool isVideo, int vcol) {
    if (vcol < 0 || vcol >= visibleColumnCount()) return ColIndex;
    return isVideo ? kV[vcol] : kA[vcol];
}

const char* FrameTableModel::visibleHeader(bool isVideo, int vcol) {
    if (vcol < 0 || vcol >= visibleColumnCount()) return "";
    return isVideo ? HV[vcol] : HA[vcol];
}

QVariant FrameTableModel::data(const QModelIndex& index, int role) const {
    if (role != Qt::DisplayRole || !index.isValid()) return {};
    const int src = sourceRow(index.row());
    if (!frames_ || src < 0 || src >= static_cast<int>(frames_->size())) return {};
    // Column is the VISIBLE column index; map it to the internal field. ColIndex
    // is the visible row number, so the filtered view stays a contiguous 1..N
    // list (and copy/paste matches what is on screen).
    return cellText((*frames_)[src], visibleToInternal(isVideo_, index.column()),
                    index.row());
}

QVariant FrameTableModel::headerData(int section, Qt::Orientation orientation,
                                     int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    if (section < 0 || section >= visibleColumnCount()) return {};
    return QString::fromLatin1(visibleHeader(isVideo_, section));
}

// ---------------- AnalysisController ----------------

AnalysisController::AnalysisController(QObject* parent)
    : QObject(parent), analyzer_(ma::infra::createProbeAnalyzer()) {
    videoModel_.setKind(true);
    audioModel_.setKind(false);
    // progressUpdated is emitted from the worker thread. AutoConnection
    // delivers it on the GUI thread, so statusText_ stays single-threaded.
    connect(this, &AnalysisController::progressUpdated, this,
            [this](int64_t packets, double seconds) {
                if (!busy_ || stopping_) return;
                statusText_ = tr("Analyzing... %1 packets (%2 s)")
                                  .arg(static_cast<qint64>(packets))
                                  .arg(seconds, 0, 'f', 1);
                emit statusTextChanged();
            });
}

AnalysisController::~AnalysisController() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

QString AnalysisController::pickMediaFile() {
    QSettings settings(QStringLiteral("MediaAnalyse"),
                       QStringLiteral("media-analyse"));
    QString dir = settings.value(QStringLiteral("lastMediaDir")).toString();
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);

    // Deliberately broad: the tool is used for troubleshooting, so an unknown
    // container/codec must still be selectable (the last filter is "All files").
    static const QString filters = QStringLiteral(
        "Media files (*.mp4 *.m4v *.mov *.mkv *.avi *.flv *.f4v *.wmv *.webm "
        "*.ts *.m2ts *.mts *.mpg *.mpeg *.3gp *.3g2 *.vob *.rm *.rmvb *.asf "
        "*.m2v *.y4m *.mxf *.m3u8 *.mpd *.sdp "
        "*.mp3 *.aac *.wav *.flac *.ogg *.oga *.opus *.m4a *.wma *.ape *.ac3 "
        "*.dts *.mka *.aiff *.aif *.caf);;"
        "Video (*.mp4 *.m4v *.mov *.mkv *.avi *.flv *.wmv *.webm *.ts *.m2ts "
        "*.mpg *.mpeg *.3gp *.vob *.rmvb *.m2v *.y4m *.mxf);;"
        "Audio (*.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a *.wma *.ac3 *.mka);;"
        "All files (*)");

    const QString path =
        QFileDialog::getOpenFileName(nullptr, tr("Select media file"), dir, filters);
    if (path.isEmpty()) return {}; // user cancelled

    settings.setValue(QStringLiteral("lastMediaDir"),
                      QFileInfo(path).absolutePath());
    // Qt already returns '/', but be explicit: FFmpeg's Windows file protocol
    // and ma::normalizeUrl() both expect forward slashes.
    return QDir::fromNativeSeparators(path);
}

static QVariantMap diagToMap(const ma::Diagnostic& d) {
    return {
        {"code", QString::fromLatin1(ma::toString(d.code))},
        {"stage", QString::fromLatin1(ma::toString(d.stage))},
        {"nativeCode", d.nativeCode},
        {"message", QString::fromStdString(d.message)},
        {"hint", QString::fromStdString(d.hint)},
        {"url", QString::fromStdString(d.url)},
        {"elapsedSeconds", d.elapsedSeconds},
    };
}

void AnalysisController::start(const QString& rawInput, const QVariantMap& options) {
    if (busy_) return;

    ma::AnalysisOptions opts;
    opts.probesize = static_cast<int64_t>(options.value("probesize").toLongLong());
    opts.analyzeduration =
        static_cast<int64_t>(options.value("analyzeduration").toLongLong());
    opts.connectTimeout = options.contains("connectTimeout")
                              ? options.value("connectTimeout").toDouble() : 15.0;
    opts.stallTimeout = options.contains("stallTimeout")
                            ? options.value("stallTimeout").toDouble() : 15.0;
    opts.probeTimeout = options.value("probeTimeout").toDouble();
    opts.maxFrames = options.contains("maxFrames")
                         ? static_cast<int64_t>(options.value("maxFrames").toLongLong())
                         : 200000;
    opts.decodeFrames = options.value("decodeFrames").toBool();
    opts.maxDecodeFrames = options.contains("maxDecodeFrames")
        ? static_cast<int64_t>(options.value("maxDecodeFrames").toLongLong())
        : 3000;
    const QVariantMap extra = options.value("extraOptions").toMap();
    for (auto it = extra.begin(); it != extra.end(); ++it)
        opts.extraOptions.emplace_back(it.key().toStdString(),
                                       it.value().toString().toStdString());

    // FFmpeg's Windows file protocol expects UTF-8. QString::toStdString()
    // uses the local 8-bit codepage (GBK on zh-CN), which corrupts non-ASCII
    // paths picked from the file dialog.
    const std::string input = rawInput.toUtf8().toStdString();
    busy_ = true;
    stopping_ = false;
    stopAtCompletion_.store(false, std::memory_order_relaxed);
    cancel_ = false;
    emit busyChanged();
    emit stoppingChanged();
    statusText_ = tr("Analyzing...");
    emit statusTextChanged();

    auto* cancel = &cancel_;
    Analyzer* analyzer = analyzer_.get();
    ProgressFn progress = [this](const ma::Progress& p) {
        emit progressUpdated(p.packetsRead, p.elapsedSeconds);
    };

    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, analyzer, input = std::move(input), opts, cancel,
                           progress = std::move(progress)]() {
        auto outcome = analyzer->analyze(input, opts, cancel, progress);
        // Snapshot the stop intent at the exact moment the worker finished.
        // A click in the small window between completion and the queued GUI
        // callback must not turn a genuine failure into "stopped by user".
        stopAtCompletion_.store(cancel->load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        // Model updates and signal consumers must run on the GUI thread.
        QMetaObject::invokeMethod(
            this,
            [this, outcome = std::move(outcome)]() mutable {
                finishWith(std::move(outcome));
            },
            Qt::QueuedConnection);
    });
}

void AnalysisController::cancel() {
    // A late click after the result was delivered is a no-op, not a new
    // analysis and not an error. The redesigned UI also keeps Stop as a
    // separate button, so this race cannot be triggered from the main action.
    if (!busy_) return;
    cancel_ = true;
    stopping_ = true;
    emit stoppingChanged();
    statusText_ = tr("Cancelling... waiting for the underlying I/O to return");
    emit statusTextChanged();
}

void AnalysisController::setFilter(bool video, const QString& text) {
    (video ? videoModel_ : audioModel_).setFilter(text);
    emit countsChanged();
}

int AnalysisController::columnCountFor(bool video) const {
    return FrameTableModel::visibleColumnCount();
}

QString AnalysisController::columnHeader(bool video, int col) const {
    return QString::fromLatin1(FrameTableModel::visibleHeader(video, col));
}

QStringList AnalysisController::columnHeaders(bool video) const {
    QStringList out;
    out.reserve(FrameTableModel::visibleColumnCount());
    for (int c = 0; c < FrameTableModel::visibleColumnCount(); ++c)
        out << QString::fromLatin1(FrameTableModel::visibleHeader(video, c));
    return out;
}

int AnalysisController::toInternalColumn(bool video, int visibleCol) const {
    return FrameTableModel::visibleToInternal(video, visibleCol);
}

int AnalysisController::typeColumn(bool video) const {
    // In the visible layout, the I/P/B "type" column is index 10 on the video
    // tab and absent (returns -1) on the audio tab.
    return video ? 10 : -1;
}

void AnalysisController::copyRows(bool video, int firstRow, int lastRow) {
    FrameTableModel& model = video ? videoModel_ : audioModel_;
    const int rowCount = model.rowCount();
    if (rowCount <= 0 || firstRow < 0 || lastRow < firstRow) return;
    lastRow = std::min(lastRow, rowCount - 1);
    if (firstRow > lastRow) return;

    QStringList lines;
    QStringList header;
    for (int c = 0; c < model.columnCount(); ++c)
        header << model.headerData(c, Qt::Horizontal, Qt::DisplayRole).toString();
    lines << header.join(QLatin1Char('\t'));
    for (int r = firstRow; r <= lastRow; ++r) {
        QStringList cells;
        for (int c = 0; c < model.columnCount(); ++c)
            cells << model.data(model.index(r, c), Qt::DisplayRole).toString();
        lines << cells.join(QLatin1Char('\t'));
    }
    if (QClipboard* cb = QGuiApplication::clipboard())
        cb->setText(lines.join(QLatin1Char('\n')));
    statusText_ = tr("Copied %1 rows to clipboard").arg(lastRow - firstRow + 1);
    emit statusTextChanged();
}

void AnalysisController::copySelection(bool video, int firstRow, int lastRow,
                                       int firstCol, int lastCol) {
    FrameTableModel& model = video ? videoModel_ : audioModel_;
    const int rowCount = model.rowCount();
    if (rowCount <= 0) return;

    firstRow = std::max(0, firstRow);
    lastRow = std::min(lastRow, rowCount - 1);
    firstCol = std::max(0, firstCol);
    lastCol = std::min(lastCol, model.columnCount() - 1);
    if (firstRow > lastRow || firstCol > lastCol) return;

    // Plain TSV with no header: pasting into Excel gives exactly the block the
    // user highlighted, matching QTableWidget's copy behaviour. model.data()
    // already resolves visible rows through the active filter.
    QStringList lines;
    lines.reserve(lastRow - firstRow + 1);
    for (int r = firstRow; r <= lastRow; ++r) {
        QStringList cells;
        cells.reserve(lastCol - firstCol + 1);
        for (int c = firstCol; c <= lastCol; ++c)
            cells << model.data(model.index(r, c), Qt::DisplayRole).toString();
        lines << cells.join(QLatin1Char('\t'));
    }
    if (QClipboard* cb = QGuiApplication::clipboard())
        cb->setText(lines.join(QLatin1Char('\n')));

    statusText_ = tr("Copied %1 x %2 cells to clipboard")
                      .arg(lastRow - firstRow + 1)
                      .arg(lastCol - firstCol + 1);
    emit statusTextChanged();
}

void AnalysisController::finishWith(ma::Outcome<ma::AnalysisResult> outcome) {
    if (worker_.joinable()) worker_.join();

    // Pressing "Stop" is a normal outcome, NEVER an error. An interrupted
    // FFmpeg read can surface as EIO / AVERROR_EXIT rather than a clean
    // Cancelled code, so a stop requested BEFORE completion also neutralises
    // those. Hard errors (bad path, invalid argument, unsupported scheme,
    // internal failure) are never swallowed, even if Stop was clicked late.
    const bool stopRequested = stopAtCompletion_.load(std::memory_order_relaxed);
    const bool hardError =
        outcome.diag.code == ma::Errc::FileNotFound ||
        outcome.diag.code == ma::Errc::InvalidArgument ||
        outcome.diag.code == ma::Errc::UnsupportedScheme ||
        outcome.diag.code == ma::Errc::Internal;
    const bool userStopped =
        (outcome.diag.code == ma::Errc::Cancelled || stopRequested) && !hardError;
    const bool failed = !outcome.ok();

    if (failed && !userStopped) {
        statusText_ = tr("Failed: %1").arg(
            QString::fromStdString(outcome.diag.message));
        emit statusTextChanged();
        emit analysisFailed(diagToMap(outcome.diag));
        busy_ = false;
        stopping_ = false;
        emit busyChanged();
        emit stoppingChanged();
        return;
    }

    // Stopped before a single frame was read (e.g. a slow HTTPS connect or
    // stream probing): keep the previous successful result instead of
    // blanking the whole UI. Still neutral - no dialog.
    if (userStopped && outcome.value.frames.empty()) {
        statusText_ = tr("Stopped by user - no frames read yet, previous result kept");
        emit statusTextChanged();
        busy_ = false;
        stopping_ = false;
        emit busyChanged();
        emit stoppingChanged();
        return;
    }

    // Normal completion, a user stop with partial frames, or a maxFrames
    // truncation: publish whatever was parsed so far. A live stream that
    // reported a clean EOF right after Stop is also Cancelled upstream now.
    ma::AnalysisResult& r = outcome.value;
    frameStore_ = std::move(r.frames);

    // Split frames by stream kind so QML tabs can bind directly.
    videoRows_.clear();
    audioRows_.clear();
    for (const auto& f : frameStore_) {
        for (const auto& s : r.streams) {
            if (s.index != f.streamIndex) continue;
            if (s.kind == ma::StreamKind::Video) videoRows_.push_back(f);
            else if (s.kind == ma::StreamKind::Audio) audioRows_.push_back(f);
            break;
        }
    }
    videoModel_.reset(&videoRows_);
    audioModel_.reset(&audioRows_);
    emit countsChanged();

    int64_t keyFrames = 0;
    int64_t decodedRows = 0;
    for (const auto& f : frameStore_) {
        if (f.pictType && (*f.pictType == "I" || *f.pictType == "K")) ++keyFrames;
        if (f.decoded) ++decodedRows;
    }
    double frameRate = 0.0;
    for (const auto& s : r.streams) {
        if (s.kind == ma::StreamKind::Video && s.frameRate > frameRate)
            frameRate = s.frameRate;
    }

    QVariantList streams;
    for (const auto& s : r.streams) {
        streams.append(QVariantMap{
            {"index", s.index},
            {"kind", QString::fromLatin1(ma::toString(s.kind))},
            {"codec", QString::fromStdString(s.codecName)},
            {"profile", QString::fromStdString(s.profile)},
            {"level", s.level},
            {"width", s.width},
            {"height", s.height},
            {"pixFmt", QString::fromStdString(s.pixFmt)},
            {"frameRate", s.frameRate},
            {"channels", s.channels},
            {"sampleRate", s.sampleRate},
            {"sampleFmt", QString::fromStdString(s.sampleFmt)},
            {"timeBase", QString::fromStdString(s.timeBase)},
            {"duration", s.duration},
        });
    }

    const bool partial = userStopped || failed || r.truncated;
    QVariantMap summary{
        {"url", QString::fromStdString(outcome.diag.url)},
        {"formatName", QString::fromStdString(r.formatName)},
        {"formatLongName", QString::fromStdString(r.formatLongName)},
        {"duration", r.duration},
        {"bitrate", static_cast<qint64>(r.bitrate)},
        {"totalPackets", static_cast<qint64>(r.totalPacketsRead)},
        {"truncated", r.truncated},
        {"partial", partial},
        {"stopped", userStopped},
        {"videoFrames", static_cast<qint64>(videoRows_.size())},
        {"audioFrames", static_cast<qint64>(audioRows_.size())},
        {"keyFrames", static_cast<qint64>(keyFrames)},
        {"decodedRows", static_cast<qint64>(decodedRows)},
        {"frameRate", frameRate},
        {"streamCount", static_cast<int>(r.streams.size())},
        {"elapsedSeconds", outcome.diag.elapsedSeconds},
        {"notes", QString::fromStdString(outcome.diag.hint)},
    };
    if (userStopped) {
        statusText_ = tr("Stopped by user - showing %1 partial rows")
                          .arg(frameStore_.size());
    } else if (r.truncated) {
        statusText_ = tr("Done (maxFrames limit) - %1 packets in %2 s")
                          .arg(r.totalPacketsRead)
                          .arg(outcome.diag.elapsedSeconds, 0, 'f', 2);
    } else {
        statusText_ = tr("Done: %1 packets in %2 s")
                          .arg(r.totalPacketsRead)
                          .arg(outcome.diag.elapsedSeconds, 0, 'f', 2);
    }
    emit statusTextChanged();
    emit analysisSucceeded(summary, streams);

    busy_ = false;
    stopping_ = false;
    emit busyChanged();
    emit stoppingChanged();
}

} // namespace ma::bridge
