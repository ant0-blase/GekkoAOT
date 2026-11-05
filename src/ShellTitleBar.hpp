#pragma once
#include <QPointer>
#include <QWidget>
class QMainWindow;
class QLabel;
class QToolButton;
class QBoxLayout;
class ShellTitleBar final : public QWidget {
public:
  explicit ShellTitleBar(QMainWindow *window);
  ~ShellTitleBar() override;
  void integrateWindowControls(QBoxLayout *row);

protected:
  bool eventFilter(QObject *, QEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;

private:
  QMainWindow *m_window;
  QLabel *m_title;
  QToolButton *m_maximize;
  QPointer<QWidget> m_outline;
  QPoint m_dragOffset;
  QPoint m_pressGlobal;
  bool m_integrated = false;
  bool m_manualDrag = false;
  bool m_restoreOnDrag = false;
  bool canDragFrom(QWidget *) const;
  void syncWindowChrome();
};
