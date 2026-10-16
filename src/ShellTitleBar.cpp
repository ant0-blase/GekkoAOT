#include "ShellTitleBar.hpp"
#include "ShellStyle.hpp"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QRegion>
#include <QTabBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QWindow>
namespace {
class WindowOutline final : public QWidget {
public:
  explicit WindowOutline(QWidget *host) : QWidget(host) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setObjectName("shellWindowOutline");
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(
        QPen(QColor(255, 255, 255, parentWidget()->isActiveWindow() ? 64 : 42),
             .75));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 38, 38);
  }
};
} // namespace
ShellTitleBar::ShellTitleBar(QMainWindow *window)
    : QWidget(window), m_window(window) {
  setObjectName("shellTitleBar");
  if (window->objectName() != "desktopGameWindow")
    m_outline = new WindowOutline(window);
  setFixedHeight(28);
  auto *row = new QHBoxLayout(this);
  row->setContentsMargins(8, 0, 0, 0);
  row->setSpacing(6);
  if (window->objectName() == "desktopGameWindow") {
    auto *icon = new QLabel(this);
    icon->setFixedSize(18, 18);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->setPixmap(window->windowIcon().pixmap(18, 18));
    row->addWidget(icon);
    connect(
        window, &QWidget::windowIconChanged, icon,
        [icon](const QIcon &value) { icon->setPixmap(value.pixmap(18, 18)); });
  }
  m_title = new QLabel(window->windowTitle());
  m_title->setFont(shellDisplayFont(13));
  m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
  row->addWidget(m_title);
  row->addStretch();
  auto add = [&](const QString &name, const QString &icon) {
    auto *b = new QToolButton(this);
    b->setAccessibleName(name);
    b->setToolTip(name);
    b->setFixedSize(32, 26);
    b->setIcon(shellIcon(icon));
    b->setIconSize({14, 14});
    row->addWidget(b);
    return b;
  };
  auto *minimize = add("Minimize", "chevron-down");
  m_maximize = add("Maximize / restore", "maximize");
  auto *close = add("Close", "x");
  close->setObjectName("windowClose");
  connect(minimize, &QToolButton::clicked, window, &QMainWindow::showMinimized);
  if (window->objectName() == "desktopGameWindow") {
    m_maximize->setAccessibleName("Fullscreen");
    m_maximize->setToolTip("Fullscreen · F11");
    connect(m_maximize, &QToolButton::clicked, window, [window] {
      window->isFullScreen() ? window->showNormal() : window->showFullScreen();
    });
  } else
    connect(m_maximize, &QToolButton::clicked, window, [window] {
      (window->isFullScreen() || window->isMaximized())
          ? window->showNormal()
          : window->showMaximized();
    });
  connect(close, &QToolButton::clicked, window, &QMainWindow::close);
  connect(window, &QWidget::windowTitleChanged, m_title, &QLabel::setText);
  setStyleSheet(
      "QWidget#shellTitleBar { background: transparent; } QLabel { color: "
      "#adadad; } QToolButton { background: transparent; color: #dedede; "
      "border: none; border-radius: 8px; padding: 0; } QToolButton:hover { "
      "background: rgba(255,255,255,18); } QToolButton:focus { border: 1px "
      "solid white; } QToolButton#windowClose:hover { background: #a43f3f; }");
  window->setWindowFlag(Qt::FramelessWindowHint);
  qApp->installEventFilter(this);
  QTimer::singleShot(0, this, [this] { syncWindowChrome(); });
}
ShellTitleBar::~ShellTitleBar() {
  if (qApp)
    qApp->removeEventFilter(this);
}
void ShellTitleBar::integrateWindowControls(QBoxLayout *row) {
  m_integrated = true;
  row->addSpacing(12);
  const auto buttons = findChildren<QToolButton *>();
  for (auto *button : buttons) {
    button->setParent(row->parentWidget());
    button->setFixedSize(32, 34);
    button->setStyleSheet(
        "QToolButton {background:transparent;border:1px solid "
        "transparent;border-radius:9px;padding:0;} QToolButton:hover "
        "{background:rgba(255,255,255,18);} QToolButton:focus "
        "{border-color:#eeeeee;} QToolButton#windowClose:hover "
        "{background:#a43f3f;}");
    row->addWidget(button);
  }
  hide();
}
void ShellTitleBar::syncWindowChrome() {
  if (!qobject_cast<QMainWindow *>(m_window))
    return;
  const bool fullscreen = m_window->isFullScreen();
  if (m_outline) {
    m_outline->setGeometry(m_window->rect());
    m_outline->setVisible(!fullscreen && !m_window->isMaximized());
    m_outline->raise();
    m_outline->update();
  }
  setVisible(!m_integrated && !fullscreen &&
             !m_window->property("tvMode").toBool());
  m_title->setVisible(!property("shellHideTitle").toBool());
  // Only the title bar takes space in windowed mode. The render surface is
  // edge to edge, and covers the entire host when the title bar is hidden.
  if (auto *root = m_window->centralWidget();
      root && root->objectName() == "gameHostRoot") {
    root->layout()->setContentsMargins(0, 0, 0, 0);
    root->layout()->setSpacing(0);
  }
  if (fullscreen || m_window->isMaximized())
    m_window->clearMask();
  else {
    QPainterPath border;
    const int radius = m_window->objectName() == "desktopGameWindow" ? 18 : 38;
    border.addRoundedRect(QRectF(m_window->rect()), radius, radius);
    m_window->setMask(QRegion(border.toFillPolygon().toPolygon()));
  }
}
bool ShellTitleBar::eventFilter(QObject *object, QEvent *event) {
  if (object == m_window && event->type() == QEvent::ActivationChange) {
    if (m_outline)
      m_outline->update();
  }
  if (object == m_window && qobject_cast<QMainWindow *>(object) &&
      (event->type() == QEvent::WindowStateChange ||
       event->type() == QEvent::Resize || event->type() == QEvent::Show ||
       event->type() == QEvent::DynamicPropertyChange)) {
    syncWindowChrome();
  }
  if (event->type() == QEvent::MouseButtonPress && !m_window->isFullScreen() &&
      !m_window->isMaximized()) {
    auto *widget = qobject_cast<QWidget *>(object);
    if (widget && widget->window() == m_window) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      const QPoint p =
          m_window->mapFromGlobal(mouse->globalPosition().toPoint());
      Qt::Edges edges;
      if (p.x() < 7)
        edges |= Qt::LeftEdge;
      else if (p.x() >= m_window->width() - 7)
        edges |= Qt::RightEdge;
      if (p.y() < 7)
        edges |= Qt::TopEdge;
      else if (p.y() >= m_window->height() - 7)
        edges |= Qt::BottomEdge;
      if (edges && mouse->button() == Qt::LeftButton &&
          m_window->windowHandle())
        return m_window->windowHandle()->startSystemResize(edges);
    }
  }
  auto *widget = qobject_cast<QWidget *>(object);
  if (widget && widget->window() == m_window) {
    if (m_manualDrag && event->type() == QEvent::MouseMove) {
      mouseMoveEvent(static_cast<QMouseEvent *>(event));
      return true;
    }
    if (m_manualDrag && event->type() == QEvent::MouseButtonRelease) {
      mouseReleaseEvent(static_cast<QMouseEvent *>(event));
      return true;
    }
    if (!m_window->isFullScreen() && canDragFrom(widget)) {
      if (event->type() == QEvent::MouseButtonPress &&
          static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        mousePressEvent(static_cast<QMouseEvent *>(event));
        return true;
      }
      if (event->type() == QEvent::MouseButtonDblClick &&
          static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        mouseDoubleClickEvent(static_cast<QMouseEvent *>(event));
        return true;
      }
    }
  }
  return QWidget::eventFilter(object, event);
}
bool ShellTitleBar::canDragFrom(QWidget *widget) const {
  if (QApplication::activePopupWidget())
    return false;
  for (auto *item = widget; item && item != m_window;
       item = item->parentWidget()) {
    if (qobject_cast<QAbstractButton *>(item) ||
        qobject_cast<QLineEdit *>(item) || qobject_cast<QComboBox *>(item) ||
        qobject_cast<QAbstractSpinBox *>(item) ||
        qobject_cast<QAbstractSlider *>(item) ||
        qobject_cast<QAbstractItemView *>(item) ||
        qobject_cast<QPlainTextEdit *>(item) ||
        qobject_cast<QTextEdit *>(item) || qobject_cast<QTabBar *>(item) ||
        item->testAttribute(Qt::WA_PaintOnScreen) ||
        item->objectName() == "desktopGameViewport" ||
        item->objectName() == "gameViewport")
      return false;
    if (auto *label = qobject_cast<QLabel *>(item);
        label &&
        label->textInteractionFlags().testFlag(Qt::TextSelectableByMouse))
      return false;
  }
  return true;
}
void ShellTitleBar::mousePressEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton || m_window->isFullScreen())
    return;
  m_dragOffset =
      event->globalPosition().toPoint() - m_window->frameGeometry().topLeft();
  m_pressGlobal = event->globalPosition().toPoint();
  m_restoreOnDrag = m_window->isMaximized();
  // XWayland can acknowledge _NET_WM_MOVERESIZE without moving a frameless
  // host containing a foreign SDL surface. Move it directly on X11.
  m_manualDrag = QGuiApplication::platformName() == "xcb" ||
                 !m_window->windowHandle() ||
                 !m_window->windowHandle()->startSystemMove();
  if (m_manualDrag)
    grabMouse();
  event->accept();
}
void ShellTitleBar::mouseMoveEvent(QMouseEvent *event) {
  if (!m_manualDrag || !event->buttons().testFlag(Qt::LeftButton))
    return;
  if (m_restoreOnDrag) {
    if ((event->globalPosition().toPoint() - m_pressGlobal).manhattanLength() <
        QApplication::startDragDistance())
      return;
    const double fraction =
        double(m_dragOffset.x()) / qMax(1, m_window->width());
    const int restoredWidth = m_window->normalGeometry().width();
    m_dragOffset = QPoint(qRound(fraction * restoredWidth),
                          qMin(m_dragOffset.y(), height() / 2));
    m_window->showNormal();
    m_restoreOnDrag = false;
  }
  if (!m_window->isMaximized())
    m_window->move(event->globalPosition().toPoint() - m_dragOffset);
}
void ShellTitleBar::mouseReleaseEvent(QMouseEvent *event) {
  if (m_manualDrag)
    releaseMouse();
  m_manualDrag = false;
  m_restoreOnDrag = false;
  event->accept();
}
void ShellTitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton)
    m_window->isMaximized() ? m_window->showNormal()
                            : m_window->showMaximized();
}
