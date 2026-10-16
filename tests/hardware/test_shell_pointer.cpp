#include "../../src/GraphicsConfigDialog.hpp"
#include "../../src/ShellStyle.hpp"
#include "../../src/ShellTitleBar.hpp"
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <QTemporaryDir>
#include <QSettings>
#include <QVBoxLayout>
#include <iostream>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " #x "\n";                                   \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

static void mouse(QWidget *widget, QEvent::Type type, QPoint local,
                  QPoint global, Qt::MouseButton button,
                  Qt::MouseButtons buttons) {
  QMouseEvent event(type, local, global, button, buttons, Qt::NoModifier);
  QApplication::sendEvent(widget, &event);
}
static void click(QWidget *widget, QPoint point) {
  const auto global = widget->mapToGlobal(point);
  mouse(widget, QEvent::MouseButtonPress, point, global, Qt::LeftButton,
        Qt::LeftButton);
  mouse(widget, QEvent::MouseButtonRelease, point, global, Qt::LeftButton,
        Qt::NoButton);
  QApplication::processEvents();
}
static void drag(QWidget *widget) {
  const QPoint point(30, 18), global = widget->mapToGlobal(point),
                              delta(45, 35);
  mouse(widget, QEvent::MouseButtonPress, point, global, Qt::LeftButton,
        Qt::LeftButton);
  mouse(widget, QEvent::MouseMove, point + delta, global + delta, Qt::NoButton,
        Qt::LeftButton);
  mouse(widget, QEvent::MouseButtonRelease, point + delta, global + delta,
        Qt::LeftButton, Qt::NoButton);
  QApplication::processEvents();
}
int main(int argc, char **argv) {
  QTemporaryDir config;qputenv("XDG_CONFIG_HOME",config.path().toUtf8());
  QApplication app(argc, argv);QSettings("GekkoAOT","GekkoAOT").setValue("ui/reducedMotion",false);
  QMainWindow host;
  host.resize(900, 700);
  auto *root = new QWidget;
  auto *layout = new QVBoxLayout(root);
  layout->addWidget(new ShellTitleBar(&host));
  auto *heading = new QLabel("Library");
  layout->addWidget(heading);
  auto *input = new QLineEdit("Search");
  layout->addWidget(input);
  auto *button = new QPushButton("Action");
  layout->addWidget(button);
  layout->addStretch();
  host.setCentralWidget(root);
  host.show();
  app.processEvents();
  auto initial = host.pos();
  drag(heading);
  CHECK(host.pos() == initial + QPoint(45, 35));
  initial = host.pos();
  drag(input);
  CHECK(host.pos() == initial);
  int actions = 0;
  QObject::connect(button, &QPushButton::clicked, [&] { ++actions; });
  click(button, button->rect().center());
  CHECK(actions == 1 && host.pos() == initial);
  QDialog drawer(&host);
  auto *body = new QVBoxLayout(&drawer);
  body->addWidget(new QPushButton("Inside"));
  auto *blank = new QWidget;blank->setMinimumHeight(40);body->addWidget(blank);
  presentShellDialog(drawer, "Settings", false);
  drawer.show();
  app.processEvents();
  click(&drawer, drawer.rect().center());
  CHECK(drawer.isVisible());
  click(button, QPoint(10, button->height() / 2));
  CHECK(!drawer.isVisible() && actions == 1);
  CHECK(host.findChild<QWidget*>("shellExitTransition"));
  drawer.show();
  app.processEvents();
  CHECK(!drawer.findChild<QWidget *>("shellDialogHeader")->isVisible());
  drag(blank);
  CHECK(host.pos() != initial && drawer.isVisible());
  drawer.reject();
  // Outside-click follows the dialog's rejection path, including Graphics
  // live-preview rollback, rather than hiding a draft that was never saved.
  GraphicsOptions options, last;
  unsigned step = 0;
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] {
    auto *panel = shellActiveDialog(&host);
    if (!panel)
      return;
    if (step++ == 0)
      panel->findChild<QDoubleSpinBox *>("brightness")->setValue(1.5);
    else {
      if (last.image.brightness != 1.5f && step < 8)
        return;
      if (last.image.brightness != 1.5f)
        std::cerr << "preview brightness=" << last.image.brightness << "\n";
      CHECK(last.image.brightness == 1.5f);
      click(root, QPoint(2, host.height() / 2));
    }
  });
  timer.start(180);
  CHECK(editGraphicsOptions(&host, options, false, [&](const auto &value) {
          last = value;
        }) == QDialog::Rejected);
  timer.stop();
  CHECK(options.image.brightness == 1 && last.image.brightness == 1);
  // Preparing a filesystem model may take longer than the whole reveal.
  // Its clock must start with the first painted frame, not during setup.
  QFileDialog browser(&host, "Open game", config.path());
  browser.setOption(QFileDialog::DontUseNativeDialog);
  presentShellDialog(browser, "Open game", false);
  const auto wait = [](int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
  };
  wait(260);
  CHECK(browser.graphicsEffect());
  browser.show();
  app.processEvents();
  const auto firstFrame = browser.grab().toImage();
  wait(70);
  CHECK(browser.graphicsEffect());
  CHECK(firstFrame != browser.grab().toImage());
  wait(200);
  CHECK(!browser.graphicsEffect());
  browser.reject();
  std::cout << "Free-area dragging, controls, outside dismissal and preview "
               "rollback; delayed file browser reveal passed\n";
}
