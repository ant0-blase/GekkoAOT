#pragma once
#include <QMenu>
class ShellMenu final : public QMenu {
public:
  explicit ShellMenu(QWidget *owner, const QPoint &origin);

protected:
  void paintEvent(QPaintEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

private:
  QPoint m_origin;
};
