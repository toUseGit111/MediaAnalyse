// Headless test of the P2 worker-isolation wiring inside the GUI controller.
// Builds a QApplication (no QML window, so it runs fine offscreen), drives
// AnalysisController::start() with a real media file, and asserts that a
// successful result is parsed from the analyzer-worker process and applied
// (analysisSucceeded fires). This exercises the full chain:
//   controller -> QProcess -> analyzer-worker -> JSON -> fromJson -> finishWith.
#include <QApplication>
#include <QTimer>
#include <QVariantMap>
#include <QObject>
#include <cstdio>

#include "ma/bridge/Bridge.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ma_controller_test <media-file>\n");
        return 2;
    }
    QApplication app(argc, argv);

    ma::bridge::AnalysisController controller;
    bool ok = false;
    std::string err;

    QObject::connect(&controller, &ma::bridge::AnalysisController::analysisSucceeded,
                     &app, [&](const QVariantMap&, const QVariantList&) {
                         ok = true;
                         app.exit(0);
                     },
                     Qt::QueuedConnection);
    QObject::connect(&controller, &ma::bridge::AnalysisController::analysisFailed,
                     &app, [&](const QVariantMap& d) {
                         ok = false;
                         err = d.value("message").toString().toStdString();
                         app.exit(1);
                     },
                     Qt::QueuedConnection);
    QTimer::singleShot(15000, &app, [&]() {
        std::fprintf(stderr, "TIMEOUT: worker did not finish\n");
        app.exit(2);
    });

    controller.start(QString::fromLocal8Bit(argv[1]), QVariantMap());
    const int rc = app.exec();
    if (rc == 0)
        std::printf("controller_test: PASS (result parsed from worker and applied)\n");
    else
        std::printf("controller_test: FAIL rc=%d err=%s\n", rc, err.c_str());
    return rc;
}
