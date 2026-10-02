#include <catch2/catch_session.hpp>
#include <QGuiApplication>

int main(int argc, char* argv[]) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("RevDash UI tests"));
    return Catch::Session().run(argc, argv);
}
