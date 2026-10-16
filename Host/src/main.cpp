#include "ui/mainwindow.h"
#include <QApplication>
#include <QFile>
#include <QStyleFactory>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MCA Studio"));
    app.setOrganizationName(QStringLiteral("MCA"));
    app.setApplicationVersion(QStringLiteral("0.2.0"));
    app.setStyle(QStyleFactory::create("Fusion"));

    // Compact professional light palette. Semantic run/pause/error colors
    // are applied by the stylesheet instead of becoming global accents.
    QPalette p;
    p.setColor(QPalette::Window, "#edf0f3");
    p.setColor(QPalette::WindowText, "#20252b");
    p.setColor(QPalette::Base, "#ffffff");
    p.setColor(QPalette::AlternateBase, "#f7f9fb");
    p.setColor(QPalette::Text, "#20252b");
    p.setColor(QPalette::Button, "#f7f8fa");
    p.setColor(QPalette::ButtonText, "#20252b");
    p.setColor(QPalette::Highlight, "#2563a6");
    p.setColor(QPalette::HighlightedText, "#ffffff");
    p.setColor(QPalette::ToolTipBase, "#20252b");
    p.setColor(QPalette::ToolTipText, "#e9edf2");
    app.setPalette(p);

    // Load global stylesheet from embedded resources
    QFile qss(":/mca.qss");
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(qss.readAll()));
        qss.close();
    }

    mca::MainWindow w;
    w.show();
    return app.exec();
}
