#include "ShellNavigation.hpp"
#include "ShellKeyboard.hpp"
#include "ShellStyle.hpp"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QVector>
#include <cmath>
#include <limits>

namespace {
QWidget *controlFor(QWidget *widget, QWidget *scope) {
  for (auto *p = widget; p && p != scope; p = p->parentWidget())
    if (qobject_cast<QAbstractSpinBox *>(p) || qobject_cast<QComboBox *>(p) ||
        qobject_cast<QAbstractItemView *>(p))
      return p;
  return widget;
}
bool acceptsFocus(QWidget *widget, QWidget *scope) {
  // Exclusive button groups remove TabFocus from unchecked buttons. They
  // still need to be reachable individually with the stick.
  const bool focusable = (widget->focusPolicy() & Qt::TabFocus) ||
                         (qobject_cast<QAbstractButton *>(widget) &&
                          widget->focusPolicy() != Qt::NoFocus);
  if (!widget->isVisibleTo(scope) || !widget->isEnabled() || !focusable ||
      widget->window() != scope->window())
    return false;
  if (controlFor(widget, scope) != widget)
    return false;
  return qobject_cast<QAbstractButton *>(widget) ||
         qobject_cast<QAbstractSpinBox *>(widget) ||
         qobject_cast<QAbstractSlider *>(widget) ||
         qobject_cast<QComboBox *>(widget) ||
         qobject_cast<QLineEdit *>(widget) ||
         qobject_cast<QAbstractItemView *>(widget) ||
         qobject_cast<QKeySequenceEdit *>(widget) ||
         qobject_cast<QPlainTextEdit *>(widget);
}
void sendKey(QWidget *widget, int key) {
  QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier),
      release(QEvent::KeyRelease, key, Qt::NoModifier);
  QApplication::sendEvent(widget, &press);
  QApplication::sendEvent(widget, &release);
}
QScrollArea *containingScroll(QWidget *widget) {
  for (auto *p = widget->parentWidget(); p; p = p->parentWidget())
    if (auto *scroll = qobject_cast<QScrollArea *>(p))
      return scroll;
  return nullptr;
}
bool visibleInScrolls(QWidget *widget, QWidget *scope) {
  QRect visible(widget->mapTo(scope, QPoint{}), widget->size());
  for (auto *p = widget->parentWidget(); p && p != scope; p = p->parentWidget())
    if (auto *scroll = qobject_cast<QScrollArea *>(p))
      visible =
          visible.intersected(QRect(scroll->viewport()->mapTo(scope, QPoint{}),
                                    scroll->viewport()->size()));
  return !visible.isEmpty();
}
} // namespace
void shellFocusControl(QWidget *widget) {
  if (!widget)
    return;
  widget->setFocus(Qt::OtherFocusReason);
  for (auto *p = widget->parentWidget(); p; p = p->parentWidget())
    if (auto *scroll = qobject_cast<QScrollArea *>(p))
      scroll->ensureWidgetVisible(widget, 14, 14);
}
bool shellMoveFocus(QWidget *scope, QWidget *current, int key) {
  const bool vertical = key == Qt::Key_Up || key == Qt::Key_Down;
  const bool forward = key == Qt::Key_Right || key == Qt::Key_Down;
  if (!vertical && key != Qt::Key_Left && key != Qt::Key_Right)
    return false;
  struct Target {
    QWidget *widget;
    QRect rect;
    double along;
  };
  QVector<Target> targets;
  auto *scroll = containingScroll(current);
  const bool innerScroll =
      scroll && scroll->objectName() != "shellSettingsScroll";
  const QPoint origin = current->mapTo(scope, current->rect().center());
  double nearestRow = std::numeric_limits<double>::max();
  for (auto *widget : scope->findChildren<QWidget *>()) {
    if (widget == current || current->isAncestorOf(widget) ||
        !acceptsFocus(widget, scope))
      continue;
    if (!(innerScroll && containingScroll(widget) == scroll) &&
        !visibleInScrolls(widget, scope))
      continue;
    QRect rect(widget->mapTo(scope, QPoint{}), widget->size());
    const QPoint delta = rect.center() - origin;
    const double along =
        (forward ? 1 : -1) * (vertical ? delta.y() : delta.x());
    if (along < qMin(vertical ? current->height() : current->width(),
                     vertical ? widget->height() : widget->width()) *
                        .5 +
                    2)
      continue;
    nearestRow = qMin(nearestRow, along);
    targets.push_back({widget, rect, along});
  }
  if (vertical && innerScroll) {
    QVector<Target> inside;
    for (const auto &target : targets)
      if (containingScroll(target.widget) == scroll)
        inside.push_back(target);
    if (!inside.isEmpty()) {
      targets = inside;
      nearestRow = std::numeric_limits<double>::max();
      for (const auto &target : targets)
        nearestRow = qMin(nearestRow, target.along);
    }
  }
  QWidget *best = nullptr;
  double bestScore = std::numeric_limits<double>::max();
  for (const auto &target : targets) {
    const auto &rect = target.rect;
    // Vertical movement selects the next row first, including controls on
    // the right of a form. Center-distance alone skipped those rows.
    if (vertical && target.along > nearestRow + 22)
      continue;
    const double across =
        vertical
            ? qMax(0, qMax(rect.left() - origin.x(), origin.x() - rect.right()))
            : qMax(0,
                   qMax(rect.top() - origin.y(), origin.y() - rect.bottom()));
    const double centerAcross = qAbs(vertical ? rect.center().x() - origin.x()
                                              : rect.center().y() - origin.y());
    const double score =
        vertical ? across * 4 + centerAcross * .1 + target.along * .01
                 : target.along + across * 8 + centerAcross * .1;
    if (score < bestScore) {
      bestScore = score;
      best = target.widget;
    }
  }
  if (best)
    shellFocusControl(best);
  return best;
}
bool shellNavigate(QWidget *scope, int key) {
  QWidget *focus = QApplication::focusWidget();
  if (!focus || (focus != scope && !scope->isAncestorOf(focus))) {
    for (auto *widget : scope->findChildren<QWidget *>())
      if (acceptsFocus(widget, scope)) {
        shellFocusControl(widget);
        focus = widget;
        break;
      }
  }
  if (!focus)
    return false;
  auto *control = controlFor(focus, scope);
  if (auto *text = qobject_cast<QPlainTextEdit *>(control);
      text && (key == Qt::Key_Up || key == Qt::Key_Down)) {
    auto *bar = text->verticalScrollBar();
    if ((key == Qt::Key_Up && bar->value() > bar->minimum()) ||
        (key == Qt::Key_Down && bar->value() < bar->maximum())) {
      bar->triggerAction(key == Qt::Key_Down
                             ? QAbstractSlider::SliderSingleStepAdd
                             : QAbstractSlider::SliderSingleStepSub);
      return true;
    }
  }
  if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
    auto *next = control;
    do {
      next = key == Qt::Key_Tab ? next->nextInFocusChain()
                                : next->previousInFocusChain();
      if (scope->isAncestorOf(next) && acceptsFocus(next, scope)) {
        shellFocusControl(next);
        return true;
      }
    } while (next != control);
    return true;
  }
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    if (auto *button = qobject_cast<QAbstractButton *>(control)) {
      button->click();
      return true;
    }
    if (auto *combo = qobject_cast<QComboBox *>(control)) {
      showShellChoices(combo);
      return true;
    }
    if (auto *edit = qobject_cast<QLineEdit *>(control);
        edit && !edit->isReadOnly()) {
      showShellKeyboard(edit);
      return true;
    }
    if (auto *spin = qobject_cast<QAbstractSpinBox *>(control)) {
      spin->stepUp();
      return true;
    }
    sendKey(control, key);
    return true;
  }
  if (key == Qt::Key_Left || key == Qt::Key_Right) {
    if (auto *slider = qobject_cast<QAbstractSlider *>(control)) {
      slider->triggerAction(key == Qt::Key_Right
                                ? QAbstractSlider::SliderSingleStepAdd
                                : QAbstractSlider::SliderSingleStepSub);
      return true;
    }
    if (auto *spin = qobject_cast<QAbstractSpinBox *>(control)) {
      key == Qt::Key_Right ? spin->stepUp() : spin->stepDown();
      return true;
    }
  }
  if (auto *view = qobject_cast<QAbstractItemView *>(control)) {
    const auto index = view->currentIndex();
    const bool edge =
        (key == Qt::Key_Up && (!index.isValid() || index.row() == 0)) ||
        (key == Qt::Key_Down && index.isValid() &&
         index.row() == view->model()->rowCount(index.parent()) - 1);
    if (!edge) {
      sendKey(view, key);
      return true;
    }
  }
  if (key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up ||
      key == Qt::Key_Down)
    return shellMoveFocus(scope, control, key);
  sendKey(control, key);
  return true;
}
