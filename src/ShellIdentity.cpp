#include "ShellStyle.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QWidget>
#ifdef GEKKOAOT_SHELL_X11
#include <QtGui/qguiapplication_platform.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
void setShellWindowIdentity(QWidget *window, const QString &suffix,
                            const QString &title, const QIcon &icon) {
  window->setWindowIcon(icon);
#ifdef Q_OS_LINUX
  const QString id = "io.github.ant0_blase.GekkoAOT" +
                     (suffix.isEmpty() ? QString{} : ".game." + suffix);
  if (QCoreApplication::applicationName() == "GekkoAOT") {
    const QString data =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString icons = data + "/icons/hicolor/256x256/apps";
    QDir().mkpath(icons);
    icon.pixmap(256, 256).save(icons + "/" + id + ".png");
    QDir().mkpath(data + "/applications");
    const QString exec = QCoreApplication::applicationFilePath()
                             .replace("%", "%%")
                             .replace("\\", "\\\\")
                             .replace("\"", "\\\"");
    const QByteArray desktop =
        ("[Desktop Entry]\nType=Application\nName=" +
         QString(title).replace('\n', ' ') + "\nExec=\"" + exec +
         "\"\nIcon=" + id + "\nStartupWMClass=" + id +
         "\nTerminal=false\nCategories=Game;\nNoDisplay=" +
         (suffix.isEmpty() ? "false" : "true") + "\n")
            .toUtf8();
    QSaveFile file(data + "/applications/" + id + ".desktop");
    if (file.open(QIODevice::WriteOnly)) {
      file.write(desktop);
      file.commit();
    }
  }
#ifdef GEKKOAOT_SHELL_X11
  if (QGuiApplication::platformName() == "xcb")
    if (auto *native =
            qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
        native && native->display()) {
      const QByteArray value = id.toUtf8();
      auto *display = native->display();
      XClassHint hint{const_cast<char *>(value.constData()),
                      const_cast<char *>(value.constData())};
      XSetClassHint(display, window->winId(), &hint);
      const Atom property = XInternAtom(display, "_GTK_APPLICATION_ID", False),
                 utf8 = XInternAtom(display, "UTF8_STRING", False);
      XChangeProperty(
          display, window->winId(), property, utf8, 8, PropModeReplace,
          reinterpret_cast<const unsigned char *>(value.constData()),
          value.size());
      XFlush(display);
    }
#endif
#else
  Q_UNUSED(suffix);
  Q_UNUSED(title);
#endif
}
