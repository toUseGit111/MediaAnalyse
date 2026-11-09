#pragma once
// ma::bridge - the ONLY layer allowed to touch both ma::core/infra and Qt.
// Converts core callbacks/results into Qt signals and QAbstractItemModels.
#include <QAbstractTableModel>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariantMap>

#include "ma/core/Analyzer.hpp"
#include "ma/core/MediaInfo.hpp"
#include "ma/core/Result.hpp"
#include "ma/core/Serialization.hpp"

namespace ma::bridge {

// Multi-column model over ma::FrameRecord rows, designed for QML TableView.
// Every field is still parsed and stored internally (15 internal columns), but
// the VISIBLE columns depend on the stream kind so the table never shows a
// meaningless column:
//   video : # pkt# stream dts dts-time dts-delta pts pts-time duration size
//           type dims format            (no samples / ch)
//   audio : # pkt# stream dts dts-time dts-delta pts pts-time duration size
//           samples format ch           (no type / dims)
// The mapping lives in visibleToInternal(); QML asks the controller for headers,
// widths and the I/P/B column via toInternalColumn() / columnHeader() / etc.
class FrameTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column {   // internal storage columns (superset of any visible set)
        ColIndex = 0,        // 1-based frame number within the current tab
        ColSeq,              // global packet/frame sequence in the whole file
        ColStream,
        ColDts,
        ColDtsTime,
        ColDtsDelta,
        ColPts,
        ColPtsTime,
        ColDuration,
        ColSize,
        ColType,             // I/P/B from packet slice header, "-" if unknown (video only)
        ColSamples,          // audio only
        ColDims,             // "WxH" - video, decode-only
        ColFormat,           // pix_fmt / sample_fmt - decode-only
        ColChannels,         // channel count - audio, decode-only
        ColumnCount,
    };

    explicit FrameTableModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    // video tab stores kind=true, audio tab kind=false (set once in the ctor).
    void setKind(bool isVideo) { isVideo_ = isVideo; }

    void reset(const std::vector<ma::FrameRecord>* frames);
    // The backing vector was appended to by the controller. Notify views with
    // a proper row insertion instead of resetting the whole model (a live
    // stream can grow to maxFrames rows). With an active filter the visible
    // mapping is rebuilt, which requires a reset instead.
    void rowsAppended(int oldVisibleCount);
    void setFilter(const QString& text);
    QString filter() const { return filter_; }

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : visibleColumnCount();
    }
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;

    // Visible rows / columns. Video and audio tabs both expose 13 visible
    // columns; visibleToInternal() maps a visible index to its internal Column.
    static int visibleColumnCount() { return 13; }
    static int visibleToInternal(bool isVideo, int visibleCol);
    static const char* visibleHeader(bool isVideo, int visibleCol);

    // Visible row -> row in frames_. -1 when out of range.
    int sourceRow(int visibleRow) const;
    // All columns joined with spaces, used by the text filter.
    QString rowText(int sourceRow) const;
    static QString cellText(const ma::FrameRecord& f, int column, int visibleRow);

private:
    void rebuildMap();

    const std::vector<ma::FrameRecord>* frames_ = nullptr;
    QString filter_;
    bool isVideo_ = true;
    std::vector<int> rowMap_;    // empty when filter_ is empty (identity)
};

// Analysis controller: owns a worker thread + cancel flag, exposes progress,
// result and diagnostics as Qt properties/signals for QML.
class AnalysisController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool stopping READ stopping NOTIFY stoppingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(int videoRows READ videoRows NOTIFY countsChanged)
    Q_PROPERTY(int audioRows READ audioRows NOTIFY countsChanged)
    Q_PROPERTY(QAbstractTableModel* videoModel READ videoModel CONSTANT)
    Q_PROPERTY(QAbstractTableModel* audioModel READ audioModel CONSTANT)

public:
    explicit AnalysisController(QObject* parent = nullptr);
    ~AnalysisController() override;

    bool busy() const { return busy_; }
    bool stopping() const { return stopping_; }
    QString statusText() const { return statusText_; }
    int videoRows() const { return videoModel_.rowCount(); }
    int audioRows() const { return audioModel_.rowCount(); }
    QAbstractTableModel* videoModel() { return &videoModel_; }
    QAbstractTableModel* audioModel() { return &audioModel_; }

    // Visible column contract for each tab. Video rows have no samples/ch, audio
    // rows have no type/dims; both expose 13 columns. QML uses these to render
    // the header, size/position columns and map a selection back to data.
    Q_INVOKABLE int columnCountFor(bool video) const;
    Q_INVOKABLE QString columnHeader(bool video, int col) const;
    Q_INVOKABLE QStringList columnHeaders(bool video) const;
    Q_INVOKABLE int toInternalColumn(bool video, int visibleCol) const;
    // Visible column index that holds I/P/B for the given tab (-1 if none).
    Q_INVOKABLE int typeColumn(bool video) const;

    Q_INVOKABLE void start(const QString& rawInput, const QVariantMap& options);
    // No-op once the worker has delivered its result: a late "stop" click must
    // never be mistaken for the start of a fresh analysis or an error.
    Q_INVOKABLE void cancel();
    // Applies a plain-text filter to one tab's model.
    Q_INVOKABLE void setFilter(bool video, const QString& text);
    // Copy a range of frame rows (inclusive) as TSV with a header line.
    Q_INVOKABLE void copyRows(bool video, int firstRow, int lastRow);
    // Copy the rectangular cell selection (inclusive row/column bounds) as
    // plain TSV - the same semantics as selecting cells in a QTableWidget.
    Q_INVOKABLE void copySelection(bool video, int firstRow, int lastRow,
                                   int firstCol, int lastCol);
    // Opens the native OS file picker. Returns an absolute local path using '/'
    // separators, or an empty string when the user cancels. The last visited
    // directory is remembered across sessions.
    Q_INVOKABLE QString pickMediaFile();

    // Diagnostics entry point for the packaged (console-less) build.
    Q_INVOKABLE QString logFolder() const;
    Q_INVOKABLE void openLogFolder();

signals:
    void busyChanged();
    void stoppingChanged();
    void statusTextChanged();
    void countsChanged();
    // Per-stream summary; QML builds info cards dynamically from this list.
    void analysisSucceeded(const QVariantMap& summary, const QVariantList& streams);
    // A real failure. The redesigned UI shows this as an inline notice bar and
    // a status-state change - never as a modal error dialog.
    void analysisFailed(const QVariantMap& diagnostic);
    void progressUpdated(int64_t packetsRead, double elapsedSeconds);
    // Incremental view of a still-running analysis (live stream). `streams` is
    // non-empty only on the first snapshot; QML must NOT reset selection,
    // filter or scroll position when handling this signal.
    void liveUpdated(const QVariantMap& summary, const QVariantList& streams);

private:
    void finishWith(ma::Outcome<ma::AnalysisResult> outcome);
    void handleLiveStreams(ma::AnalysisResult& r);
    void handleLiveFrames(ma::AnalysisResult& r);
    QVariantMap liveSummary(bool withStreamCount = false) const;

    // ---- out-of-process analysis (P2: worker isolation) ----
    void launchWorker(const std::string& req);
    void onWorkerOutput();
    void onWorkerError();
    void onWorkerFinished(int exitCode, QProcess::ExitStatus status);
    void handleWorkerLine(const std::string& line);
    void handleWorkerCrash();
    QString newestCrashDump() const;

    QProcess worker_;                // analyzer-worker.exe, crash-isolated
    std::string lastReq_;            // last request sent (for auto-restart)
    std::string workerStdoutBuffer_;
    std::string workerStderrBuffer_;
    bool gotResult_ = false;         // a final "result" line was received
    bool userRequestedStop_ = false; // Stop clicked while busy
    int crashRestartCount_ = 0;      // capped auto-restart on worker crash
    int progressLogTick_ = 0;        // throttle progress log lines
    QString workerPath_;             // path to analyzer-worker.exe
    QString lastDumpPath_;           // dump reported by the worker on stderr

    // ---- live incremental publishing (worker "streams"/"frames" lines) ----
    ma::AnalysisResult liveHeader_;             // container + stream list
    std::vector<ma::StreamInfo> liveStreams_;   // kind lookup for new rows
    QVariantList liveStreamVariants_;
    bool liveFramesStarted_ = false;            // first batch replaces old rows
    int64_t livePackets_ = 0;
    double liveElapsed_ = 0.0;
    int64_t liveKeyFrames_ = 0;
    int64_t liveDecodedRows_ = 0;
    int snapshotLogTick_ = 0;

    std::vector<ma::FrameRecord> frameStore_; // owned, models point into it
    std::vector<ma::FrameRecord> videoRows_;  // per-kind splits (owned)
    std::vector<ma::FrameRecord> audioRows_;
    FrameTableModel videoModel_;
    FrameTableModel audioModel_;
    bool busy_ = false;
    bool stopping_ = false;
    QString statusText_ = tr("Ready");
};

} // namespace ma::bridge
