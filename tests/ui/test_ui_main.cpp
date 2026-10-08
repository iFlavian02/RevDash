#include <catch2/catch_session.hpp>
#include <QGuiApplication>

int main(int argc, char* argv[]) {
#ifdef Q_OS_WIN
    // The Windows Qt deployment provides qwindows; an offscreen platform
    // plugin is not part of this installation. Windows can still create the
    // test windows without showing them because each test owns their lifetime.
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows"));
#else
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#endif
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("RevDash UI tests"));
    return Catch::Session().run(argc, argv);
}
