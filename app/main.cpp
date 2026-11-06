// media-analyse - QML application entry point.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtGlobal>

#include <cstdio>
#include <cstring>

#include "ma/bridge/Bridge.hpp"
#include "ma/infra/CrashHandler.hpp"
#include "ma/infra/Ffmpeg.hpp"
#include "ma/infra/Interrupt.hpp"
#include "ma/infra/Logger.hpp"

namespace {

// The shipped build is a WIN32 subsystem executable: without this handler all
// Qt/QML warnings (including "ReferenceError" and QML file:line diagnostics)
// evaporate. Route them to the persistent log as well as to stderr when a
// console happens to be attached.
void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx,
                      const QString& msg) {
    ma::infra::Severity sev = ma::infra::Severity::Debug;
    switch (type) {
    case QtDebugMsg: sev = ma::infra::Severity::Debug; break;
    case QtInfoMsg: sev = ma::infra::Severity::Info; break;
    case QtWarningMsg: sev = ma::infra::Severity::Warning; break;
    case QtCriticalMsg: sev = ma::infra::Severity::Error; break;
    case QtFatalMsg: sev = ma::infra::Severity::Fatal; break;
    }
    QString text = msg;
    if (ctx.file && *ctx.file)
        text += QStringLiteral(" @ %1:%2")
                    .arg(QString::fromUtf8(ctx.file))
                    .arg(ctx.line);
    if (ctx.category && std::strcmp(ctx.category, "default") != 0)
        text += QStringLiteral(" [%1]").arg(QString::fromUtf8(ctx.category));
    const std::string utf8 = text.toUtf8().toStdString();
    ma::infra::logMessage(sev, "qt", ma::infra::redact(utf8));
    if (sev >= ma::infra::Severity::Error)
        std::fprintf(stderr, "[qt] %s\n", utf8.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    // Observability first: the log must exist before anything can fail, and the
    // crash handler logs the dump path through that same logger.
    ma::infra::initLogger("media-analyse");
    ma::infra::installCrashHandler("media-analyse");
    qInstallMessageHandler(&qtMessageHandler);

    // The PRISM theme restyles every control; Basic is the neutral base so no
    // platform/Material palette bleeds through the custom backgrounds.
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    // QApplication (not QGuiApplication) is required so QFileDialog can show
    // the native Windows file picker.
    QApplication app(argc, argv);
    QApplication::setApplicationName("media-analyse");
    QApplication::setApplicationVersion("0.1.0");

    MA_LOGI("app", "session start v"
                       << QApplication::applicationVersion().toUtf8().constData()
                       << " Qt=" << qVersion()
                       << " FFmpeg=" << ma::infra::ffmpegVersionString()
                       << " pid=" << QCoreApplication::applicationPid());
    MA_LOGI("app", "exe=" << QCoreApplication::applicationFilePath().toUtf8().constData());
    MA_LOGI("app", "cwd=" << QDir::currentPath().toUtf8().constData());
    MA_LOGI("app", "log=" << ma::infra::logFilePath());
    MA_LOGI("app", "args=" << ma::infra::redact(
                       app.arguments().join(QLatin1Char(' ')).toUtf8().toStdString()));

    ma::infra::initFfmpeg();

    qmlRegisterType<ma::bridge::FrameTableModel>("MediaAnalyse", 1, 0,
                                                 "FrameTableModel");

    QQmlApplicationEngine engine;
    ma::bridge::AnalysisController controller;
    engine.rootContext()->setContextProperty("Analysis", &controller);
    // Optional CLI argument: open and analyze this path immediately. A bare
    // relative path must be resolved here: once the GUI runs from a shortcut or
    // double-click its working directory is the exe directory, so "clip.mp4"
    // would otherwise fail with file-not-found. URLs are passed through as-is.
    QString initial;
    if (argc > 1) {
        initial = QString::fromLocal8Bit(argv[1]);
        const bool isUrl = initial.contains(QLatin1String("://")) ||
                           initial.startsWith(QLatin1String("file:"));
        if (!isUrl) initial = QFileInfo(initial).absoluteFilePath();
    }
    engine.rootContext()->setContextProperty("initialInput", initial);
    if (!initial.isEmpty())
        MA_LOGI("app", "initial input=" << ma::infra::redact(
                           initial.toUtf8().toStdString()));

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() {
            // Qt emits the actual QML errors through qWarning(), which the
            // message handler above has already written to the log file.
            MA_LOGE("qml", "QML object creation failed - exiting");
            QCoreApplication::exit(1);
        }, Qt::QueuedConnection);
    engine.loadFromModule("MediaAnalyse", "Main");
    const int rc = app.exec();
    MA_LOGI("app", "session end rc=" << rc);
    ma::infra::closeLogger();
    return rc;
}
