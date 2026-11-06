// media-analyse - QML application entry point.
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "ma/bridge/Bridge.hpp"
#include "ma/infra/Interrupt.hpp"

int main(int argc, char** argv) {
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
    // Optional CLI argument: open and analyze this path immediately.
    engine.rootContext()->setContextProperty(
        "initialInput", argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString());

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("MediaAnalyse", "Main");
    return app.exec();
}
