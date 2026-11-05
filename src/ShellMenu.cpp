#include "ShellMenu.hpp"
#include "ShellStyle.hpp"
#include <QAction>
#include <QHideEvent>
#include <QPainter>
#include <QProxyStyle>
#include <QShowEvent>
#include <QStyleFactory>
#include <QStyleOptionMenuItem>

ShellMenu::ShellMenu(QWidget *owner, const QPoint &origin)
    : QMenu(owner), m_origin(origin) {
  setObjectName("shellContextMenu");
  setMinimumWidth(280);
  setAttribute(Qt::WA_TranslucentBackground);
  // Rows are painted as a single unit: hover, icon and text share one inset.
  setStyleSheet(
      "QMenu {background:transparent;border:none;padding:10px "
      "0;min-width:280px;} QMenu::item "
      "{min-height:22px;padding:10px 20px;margin:0;border:none;} "
      "QMenu::item:selected "
      "{background:transparent;} QMenu::separator {height:12px;margin:0;}");
}
void ShellMenu::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.setBrush(QColor("#282828"));
  p.setPen(QPen(QColor(255, 255, 255, 62), .5));
  p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 18, 18);
  for (auto *action : actions()) {
    if (!action->isVisible())
      continue;
    const auto row = actionGeometry(action);
    if (action->isSeparator()) {
      p.setPen(QColor(255, 255, 255, 20));
      p.drawLine(row.left() + 20, row.center().y(), row.right() - 20,
                 row.center().y());
      continue;
    }
    if (action == activeAction()) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(255, 255, 255, 17));
      p.drawRoundedRect(QRectF(row).adjusted(8, 2, -8, -2), 10, 10);
    }
    p.setOpacity(action->isEnabled() ? 1 : .4);
    action->icon().paint(&p,
                         QRect(row.left() + 20, row.center().y() - 10, 20, 20));
    p.setPen(QColor("#eeeeee"));
    const QString shortcut =
        action->shortcut().toString(QKeySequence::NativeText);
    const int suffix = shortcut.isEmpty()
                           ? 0
                           : p.fontMetrics().horizontalAdvance(shortcut) + 22;
    const QRect label(row.left() + 54, row.top(), row.width() - 74 - suffix,
                      row.height());
    p.drawText(label, Qt::AlignVCenter,
               p.fontMetrics().elidedText(action->text().remove('&'),
                                          Qt::ElideRight, label.width()));
    if (!shortcut.isEmpty()) {
      p.setPen(QColor("#aaaaaa"));
      p.drawText(row.adjusted(54, 0, -20, 0), Qt::AlignRight | Qt::AlignVCenter,
                 shortcut);
    }
    p.setOpacity(1);
  }
}
void ShellMenu::showEvent(QShowEvent *event) {
  QMenu::showEvent(event);
  setProperty("shellExitCaptured", false);
  shellRevealFrom(this, m_origin);
}
void ShellMenu::hideEvent(QHideEvent *event) {
  shellRetire(this);
  QMenu::hideEvent(event);
}
