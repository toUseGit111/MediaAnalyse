// media-analyse - QML application entry point.
#include <QApplication>
#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "ma/bridge/Bridge.hpp"
#include "ma/infra/CrashHandler.hpp"
#include "ma/infra/Interrupt.hpp"

int main(int argc, char** argv) {
    // Install a minidump crash handler first: if THIS process ever faults the
    // GUI itself dumps to <exedir>/crashes instead of vanishing. (The analysis
    // kernel's faults are isolated in analyzer-worker.exe - see P2.)
    ma::infra::installCrashHandler("media-analyse");

    // The PRISM theme restyles every control; Basic is the neutral base so no
    // platform/Material palette bleeds through the custom backgrounds.
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    // QApplication (not QGuiApplication) is required so QFileDialog can show
    // the native Windows file picker.
    QApplication app(argc, argv);
    QApplication::setApplicationName("media-analyse");
    QApplication::setApplicationVersion("0.1.0");

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

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("MediaAnalyse", "Main");
    return app.exec();
}
