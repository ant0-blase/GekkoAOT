#include "MainWindow.hpp"

#include <QApplication>
#include <QByteArray>
#include <QGuiApplication>
#include <QStyleFactory>

int main(int argc, char** argv)
{
    // Preserve fractional monitor scales such as GNOME 125/150/175% instead of
    // rounding them to 1x or 2x. Qt 6 is high-DPI aware by default; this policy
    // controls only fractional rounding and therefore works on both X11 and
    // Wayland backends.
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

#ifdef Q_OS_LINUX
    if (qEnvironmentVariableIsEmpty("QT_SCALE_FACTOR_ROUNDING_POLICY"))
        qputenv("QT_SCALE_FACTOR_ROUNDING_POLICY", QByteArrayLiteral("PassThrough"));

    // A surface created by one Wayland client cannot be reparented into another
    // client's Qt widget. When XWayland is available, use xcb for GekkoAOT so
    // AuroraGX can present into the exact GameView child XID. This remains a
    // Wayland-session-compatible path: Mutter/KWin owns scaling/composition.
    // On a pure Wayland session without DISPLAY we leave Qt native Wayland and
    // the runtime falls back to its own Wayland top-level window.
    // The native GX renderer can only share the exact Qt GameView surface
    // across processes through X11/XWayland.  v42 used one XID from window
    // creation onward; preserve that behaviour instead of creating an Aurora
    // top-level and trying to reparent it after Vulkan has attached a surface.
    const QByteArray singleWindow = qgetenv("GEKKOAOT_NATIVE_GX_SINGLE_WINDOW");
    const bool wantSingleWindow = singleWindow.isEmpty() || singleWindow != "0";
    if (wantSingleWindow &&
        qEnvironmentVariable("XDG_SESSION_TYPE").compare("wayland", Qt::CaseInsensitive) == 0 &&
        !qEnvironmentVariableIsEmpty("DISPLAY"))
    {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("xcb"));
    }
#endif

    QApplication app(argc, argv);
    app.setApplicationName("GekkoAOT");
    app.setOrganizationName("GekkoAOT");
    app.setStyle(QStyleFactory::create("Fusion"));

    MainWindow window;
    window.show();
    return app.exec();
}
