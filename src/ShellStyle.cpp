#include "ShellStyle.hpp"
#include "SettingsStyle.hpp"
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsEffect>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <cmath>
#include <memory>

class ShellFileIcons final : public QFileIconProvider {
public:
  QIcon icon(const QFileInfo &info) const override {
    return shellIcon(info.isDir() ? "folder-open" : "disc-3");
  }
  QIcon icon(IconType type) const override {
    return shellIcon(type == Folder ? "folder-open" : "disc-3");
  }
};

bool shellTVMode(const QWidget *widget) {
  for (auto *current = widget; current; current = current->parentWidget())
    if (current->property("tvMode").toBool())
      return true;
  return false;
}

static void placeShellDialog(QDialog *dialog) {
  auto *host = dialog->parentWidget();
  if (!host)
    return;
  const bool tv = dialog->property("tvMode").toBool(),
             drawer = dialog->property("shellDrawer").toBool();
  const int margin = tv ? 32 : 20;
  if (drawer) {
    const bool navigation = dialog->property("shellNavigationPanel").toBool();
    if (dialog->property("shellChoicePanel").toBool()) {
      const auto anchor = dialog->property("shellChoiceAnchor").toRect();
      const QPoint point = host->mapFromGlobal(anchor.topLeft());
      const int w = qMin(qMax(280, anchor.width()),
                         qMin(620, host->width() - margin * 2));
      const int h = qMin(dialog->findChild<QWidget *>("shellSettingsCard")
                                 ->sizeHint()
                                 .height() +
                             28,
                         host->height() - margin * 2);
      const int y =
          point.y() + anchor.height() + 8 + h <= host->height() - margin
              ? point.y() + anchor.height() + 8
              : qMax(margin, point.y() - h - 8);
      dialog->setGeometry(qBound(margin, point.x(), host->width() - w - margin),
                          y, w, h);
      dialog->layout()->setContentsMargins(14, 14, 14, 14);
      QPainterPath shape;
      shape.addRoundedRect(QRectF(dialog->rect()), 22, 22);
      dialog->setMask(QRegion(shape.toFillPolygon().toPolygon()));
      return;
    }
    const int width = qMin(navigation ? 320 : 520, host->width() - margin * 2);
    int top = qMin(tv ? 96 : 110, host->height() / 5);
    if (navigation && !tv)
      if (auto *navbar = host->findChild<QWidget *>("desktopNavigation"))
        top = navbar->mapTo(host, QPoint(0, navbar->height())).y() + 8;
    const int x = dialog->property("shellDrawerLeft").toBool()
                      ? margin
                      : host->width() - width - margin;
    auto *card = dialog->findChild<QWidget *>("shellSettingsCard");
    const int preferredHeight = navigation
                                    ? qMax(card->sizeHint().height(),
                                           card->minimumSizeHint().height()) +
                                          28
                                    : host->height();
    dialog->setGeometry(x, top, width,
                        qMin(preferredHeight, host->height() - top - margin));
    dialog->layout()->setContentsMargins(14, 14, 14, 14);
    QPainterPath shape;
    shape.addRoundedRect(QRectF(dialog->rect()), 22, 22);
    dialog->setMask(QRegion(shape.toFillPolygon().toPolygon()));
  } else {
    dialog->clearMask();
    dialog->setGeometry(host->rect());
    const int width = qMin(host->width() - margin * 2, 1100);
    dialog->layout()->setContentsMargins(
        qMax(margin, (host->width() - width) / 2), margin,
        qMax(margin, (host->width() - width) / 2), margin);
  }
}

class SettingsBackdrop final : public QWidget {
public:
  explicit SettingsBackdrop(QDialog *dialog) : QWidget(dialog) {
    setObjectName("shellDialogBackdrop");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    dialog->installEventFilter(this);
    qApp->installEventFilter(this);
    if (dialog->parentWidget())
      dialog->parentWidget()->installEventFilter(this);
  }
  QImage image;
  QPointer<QWidget> previousFocus;

protected:
  bool eventFilter(QObject *watched, QEvent *e) override {
    auto *dialog = static_cast<QDialog *>(parentWidget());
    if (e->type() == QEvent::MouseButtonPress && dialog->isVisible() &&
        dialog->parentWidget() && !QApplication::activePopupWidget() &&
        shellActiveDialog(dialog->parentWidget()) == dialog) {
      auto *target = qobject_cast<QWidget *>(watched);
      auto *mouse = static_cast<QMouseEvent *>(e);
      auto *host = dialog->parentWidget();
      const QPoint global = mouse->globalPosition().toPoint();
      if (target && target->window() == host->window() &&
          mouse->button() == Qt::LeftButton &&
          host->rect().contains(host->mapFromGlobal(global))) {
        bool inside = false;
        if (dialog->property("shellDrawer").toBool()) {
          const QPoint local = dialog->mapFromGlobal(global);
          inside = dialog->rect().contains(local) &&
                   (dialog->mask().isEmpty() || dialog->mask().contains(local));
        } else {
          for (const auto *name : {"shellDialogHeader", "shellSettingsScroll"})
            if (auto *content = dialog->findChild<QWidget *>(name))
              inside |=
                  content->isVisible() &&
                  content->rect().contains(content->mapFromGlobal(global));
        }
        if (!inside) {
          // Consume the press so the control behind a dismissed panel cannot
          // also launch a game or open another action with the same click.
          dialog->reject();
          return true;
        }
      }
    }
    if (watched == dialog->parentWidget() && e->type() == QEvent::Resize &&
        dialog->isVisible())
      placeShellDialog(dialog);
    if (watched == dialog && e->type() == QEvent::Resize) {
      setGeometry(dialog->rect());
      lower();
    }
    if (watched == dialog && e->type() == QEvent::Show) {
      dialog->setProperty("shellExitCaptured", false);
      previousFocus = QApplication::focusWidget();
      dialog->raise();
    }
    if (watched == dialog && e->type() == QEvent::Hide) {
      shellRetire(dialog);
      if (previousFocus)
        previousFocus->setFocus();
    }
    return QWidget::eventFilter(watched, e);
  }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool drawer = parentWidget()->property("shellDrawer").toBool();
    if (drawer) {
      QPainterPath shape;
      shape.addRoundedRect(QRectF(rect()), 22, 22);
      p.setClipPath(shape);
    }
    p.fillRect(rect(), QColor("#282828"));
    if (!image.isNull()) {
      p.setOpacity(.06);
      if (drawer)
        p.drawImage(rect(), image, QRectF(parentWidget()->geometry()));
      else
        p.drawImage(rect(), image);
      p.setOpacity(1);
    }
    if (drawer) {
      p.setPen(QPen(QColor(255, 255, 255, 62), .5));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 22, 22);
    }
  }
};

namespace {
class ChoiceEvents final : public QObject {
public:
  explicit ChoiceEvents(QComboBox *combo) : QObject(combo) {}

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    auto *combo = static_cast<QComboBox *>(object);
    if (event->type() == QEvent::MouseButtonPress &&
        static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
      showShellChoices(combo);
      return true;
    }
    if (event->type() == QEvent::KeyPress) {
      const auto key = static_cast<QKeyEvent *>(event)->key();
      if (key == Qt::Key_Space || key == Qt::Key_Return ||
          key == Qt::Key_Enter || key == Qt::Key_F4) {
        showShellChoices(combo);
        return true;
      }
    }
    return QObject::eventFilter(object, event);
  }
};
} // namespace
void presentShellDialog(QDialog &dialog, const QString &heading, bool tvMode) {
  // Overlays share the launcher coordinate space. A dialog parented to a
  // sidebar or another overlay must never become a small nested "window".
  if (auto *parent = dialog.parentWidget();
      parent && parent != parent->window())
    dialog.setParent(parent->window(), Qt::Widget);
  if (auto *files = qobject_cast<QFileDialog *>(&dialog)) {
    files->setViewMode(QFileDialog::Detail);
    auto provider = std::make_shared<ShellFileIcons>();
    files->setIconProvider(provider.get());
    QObject::connect(files, &QObject::destroyed, [provider] {});
    QTimer::singleShot(0, files, [files] {
      if (auto *tree = files->findChild<QTreeView *>("treeView")) {
        tree->setFocus();
        if (tree->model() && tree->model()->rowCount(tree->rootIndex()))
          tree->setCurrentIndex(tree->model()->index(0, 0, tree->rootIndex()));
      }
    });
  }
  auto *card = dialog.findChild<QWidget *>("shellSettingsCard");
  if (!card) {
    dialog.setProperty("shellOriginalSize", dialog.size());
    dialog.setProperty("shellOriginalMinimum", dialog.minimumSize());
    auto *old = dialog.layout();
    card = new QWidget(&dialog);
    card->setObjectName("shellSettingsCard");
    if (old)
      card->setLayout(old);
    auto *outer = new QVBoxLayout(&dialog);
    outer->setSpacing(16);
    auto *header = new QWidget(&dialog);
    header->setObjectName("shellDialogHeader");
    auto *line = new QHBoxLayout(header);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(16);
    auto *back = new QPushButton(shellIcon("arrow-left"), "Back", header);
    back->setObjectName("shellDialogBack");
    QObject::connect(back, &QPushButton::clicked, &dialog, &QDialog::reject);
    line->addWidget(back);
    auto *title = new QLabel(heading, header);
    title->setObjectName("shellDialogHeading");
    line->addWidget(title);
    line->addStretch();
    outer->addWidget(header);
    auto *scroll = new QScrollArea(&dialog);
    scroll->setObjectName("shellSettingsScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(card);
    outer->addWidget(scroll, 1);
    auto *hint = new QLabel(
        "Joystick  Navigate    A / ×  Select    B / ○  Back", &dialog);
    hint->setObjectName("shellDialogHint");
    hint->setAlignment(Qt::AlignRight);
    outer->addWidget(hint);
    auto *backdrop = new SettingsBackdrop(&dialog);
    backdrop->lower();
  }
  for (auto *scroll : dialog.findChildren<QScrollArea *>()) {
    scroll->viewport()->setProperty("shellPage", true);
    scroll->viewport()->setAutoFillBackground(false);
    if (scroll->widget() &&
        scroll->widget()->objectName() != "shellSettingsCard") {
      scroll->widget()->setProperty("shellPage", true);
      scroll->widget()->setAutoFillBackground(false);
    }
  }
  for (auto *spin : dialog.findChildren<QAbstractSpinBox *>())
    if (!spin->property("shellStepper").toBool()) {
      auto *wrapper = new QWidget(spin->parentWidget());
      wrapper->setObjectName("shellStepper");
      auto *ownerLayout = spin->parentWidget()->layout();
      if (auto *item = ownerLayout
                           ? ownerLayout->replaceWidget(
                                 spin, wrapper, Qt::FindChildrenRecursively)
                           : nullptr) {
        delete item;
        auto *line = new QHBoxLayout(wrapper);
        line->setContentsMargins(3, 2, 3, 2);
        line->setSpacing(2);
        auto *minus = new QPushButton("−");
        auto *plus = new QPushButton("+");
        minus->setObjectName("stepperMinus");
        plus->setObjectName("stepperPlus");
        minus->setAccessibleName("Decrease");
        plus->setAccessibleName("Increase");
        for (auto *b : {minus, plus}) {
          b->setFixedSize(32, 34);
          b->setAutoRepeat(true);
          b->setAutoDefault(false);
        }
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setProperty("shellStepper", true);
        spin->setFocusPolicy(Qt::ClickFocus);
        // Keep the spin box as the value/validation model. A plain label
        // avoids platform spinbox subcontrol geometry clipping the text
        // after removing its native arrows and reparenting it.
        auto *value = new QLabel(spin->text(), wrapper);
        value->setObjectName("shellStepperValue");
        value->setAlignment(Qt::AlignCenter);
        value->setMinimumWidth(54);
        value->setTextFormat(Qt::PlainText);
        spin->setParent(wrapper);
        spin->hide();
        if (auto *edit = spin->findChild<QLineEdit *>())
          QObject::connect(edit, &QLineEdit::textChanged, value,
                           &QLabel::setText);
        if (auto *integer = qobject_cast<QSpinBox *>(spin))
          QObject::connect(integer, &QSpinBox::textChanged, value,
                           &QLabel::setText);
        if (auto *real = qobject_cast<QDoubleSpinBox *>(spin))
          QObject::connect(real, &QDoubleSpinBox::textChanged, value,
                           &QLabel::setText);
        line->addWidget(minus);
        line->addWidget(value, 1);
        line->addWidget(plus);
        QObject::connect(minus, &QPushButton::clicked, spin,
                         &QAbstractSpinBox::stepDown);
        QObject::connect(plus, &QPushButton::clicked, spin,
                         &QAbstractSpinBox::stepUp);
        // A sidebar can have its own button stylesheet. Keep this control
        // self-contained so inherited padding cannot consume its symbols.
        wrapper->setStyleSheet(R"(
                QWidget#shellStepper { background: rgba(255,255,255,9); border: 1px solid rgba(255,255,255,32); border-radius: 12px; }
                QWidget#shellStepper QAbstractSpinBox { color: #eeeeee; background: transparent; border: none; padding: 0; min-height: 30px; }
                QWidget#shellStepper QLineEdit { color: #eeeeee; background: transparent; border: none; padding: 0; min-height: 26px; }
                QLabel#shellStepperValue { color: #eeeeee; background: transparent; border: none; padding: 0 4px; font-size: 15px; }
                QWidget#shellStepper QPushButton { color: #eeeeee; background: transparent; border: 1px solid transparent; padding: 0; margin: 0; min-height: 0; font-size: 20px; border-radius: 8px; }
                QWidget#shellStepper QPushButton:hover { background: rgba(255,255,255,14); }
                QWidget#shellStepper QPushButton:focus { background: rgba(255,255,255,20); border-color: #eeeeee; }
            )");
        // replaceWidget does not show a new wrapper in an already visible
        // sidebar; show it explicitly (hidden ancestor pages stay hidden).
        value->show();
        minus->show();
        plus->show();
        wrapper->show();
      } else
        delete wrapper;
    }
  for (auto *tabs : dialog.findChildren<QTabWidget *>()) {
    for (int i = 0; i < tabs->count(); ++i) {
      tabs->widget(i)->setProperty("shellPage", true);
      tabs->widget(i)->setAutoFillBackground(false);
    }
    if (tabs->property("shellTabs").toBool())
      continue;
    auto *parentLayout =
        qobject_cast<QBoxLayout *>(tabs->parentWidget()->layout());
    if (!parentLayout)
      continue;
    auto *strip = new QWidget(tabs->parentWidget());
    strip->setObjectName("shellTabs");
    auto *row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    auto *group = new QButtonGroup(strip);
    group->setExclusive(true);
    for (int i = 0; i < tabs->count(); ++i) {
      auto *button = new QPushButton(tabs->tabText(i));
      button->setObjectName("shellTab" + QString::number(i));
      button->setProperty("shellTab", true);
      button->setCheckable(true);
      button->setChecked(i == tabs->currentIndex());
      button->setAutoDefault(false);
      group->addButton(button, i);
      row->addWidget(button);
      QObject::connect(button, &QPushButton::clicked, tabs,
                       [tabs, i] { tabs->setCurrentIndex(i); });
    }
    row->addStretch();
    QObject::connect(tabs, &QTabWidget::currentChanged, group,
                     [group](int index) {
                       if (auto *button = group->button(index))
                         button->setChecked(true);
                     });
    parentLayout->insertWidget(parentLayout->indexOf(tabs), strip);
    tabs->tabBar()->hide();
    tabs->setProperty("shellTabs", true);
  }
  dialog.setProperty("tvMode", tvMode);
  dialog.setProperty("shellNavigationPanel",
                     heading == "Menu" || heading == "Settings" ||
                         dialog.property("shellChoicePanel").toBool());
  dialog.findChild<QWidget *>("shellDialogHeader")
      ->setVisible(!dialog.property("shellNavigationPanel").toBool());
  auto *title = dialog.findChild<QLabel *>("shellDialogHeading");
  title->setText(heading.toUpper());
  title->setFont(shellDisplayFont(tvMode ? 28 : 22));
  title->setStyleSheet(
      QString("font-size: %1px; color: #f1f1f1;").arg(tvMode ? 28 : 22));
  dialog.findChild<QLabel *>("shellDialogHint")
      ->setVisible(tvMode && !dialog.property("shellNavigationPanel").toBool());
  auto *backdrop = static_cast<SettingsBackdrop *>(
      dialog.findChild<QWidget *>("shellDialogBackdrop"));
  // Snapshot the launcher itself before showing the child. The blur is
  // cached at opening, independent of the desktop compositor or platform.
  if (auto *host = dialog.parentWidget()) {
    const bool wasVisible = dialog.isVisible();
    if (wasVisible) {
      dialog.setProperty("shellSuppressExit", true);
      dialog.hide();
      dialog.setProperty("shellSuppressExit", false);
    }
    const QImage source = host->grab().toImage();
    backdrop->image =
        source
            .scaled(qMax(1, host->width() / 10), qMax(1, host->height() / 10),
                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .scaled(host->size(), Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation);
    dialog.setWindowFlags(Qt::Widget);
    dialog.setMinimumSize(0, 0);
    dialog.setProperty("shellDrawer",
                       heading == "Compilations" || heading == "Menu" ||
                           heading == "Settings" ||
                           dialog.property("shellChoicePanel").toBool());
    dialog.setProperty("shellDrawerLeft", heading == "Menu");
    placeShellDialog(&dialog);
  }
  dialog.setStyleSheet(settingsStyleSheet() + QString(R"(
        QDialog { background: transparent; }
        QWidget#shellSettingsCard { background: #282828; border: 1px solid rgba(255,255,255,22); border-radius: 22px; }
        QDialog[shellDrawer="true"] QWidget#shellSettingsCard { background: transparent; border: none; }
        QWidget[shellPage="true"], QStackedWidget { background: transparent; }
        QWidget#shellStepper { background: rgba(255,255,255,9); border: 1px solid rgba(255,255,255,32); border-radius: 12px; }
        QWidget#shellStepper QAbstractSpinBox { border: none; background: transparent; padding: 0 2px; min-height: 28px; }
        QWidget#shellStepper QPushButton { border: none; background: transparent; font-size: 20px; padding: 0; border-radius: 8px; min-height: 28px; }
        QWidget#shellStepper QPushButton:hover, QWidget#shellStepper QPushButton:focus { background: rgba(255,255,255,20); }
        QSlider:focus { border: 1px solid rgba(255,255,255,120); border-radius: 10px; }
        QCheckBox:focus { border: 1px solid rgba(255,255,255,120); border-radius: 6px; }
        QTabWidget::pane { background: transparent; border: none; }
        QWidget#shellTabs { background: transparent; }
        QPushButton[shellTab="true"] { background: transparent; border: 1px solid transparent; border-radius: 12px; padding: 8px 18px; color: #aaaaaa; }
        QPushButton[shellTab="true"]:checked { color: #eeeeee; background: rgba(255,255,255,12); border-color: rgba(255,255,255,24); }
        QPushButton[shellTab="true"]:focus { color: white; border-color: #eeeeee; }
        QGroupBox { background: rgba(255,255,255,8); border-color: rgba(255,255,255,18); }
        QScrollArea#shellSettingsScroll, QScrollArea#shellSettingsScroll > QWidget { background: transparent; }
        QWidget#shellDialogHeader { background: transparent; }
        QLabel#shellDialogHint { color: #ababab; }
        QTabBar::tab { border: 1px solid transparent; border-radius: 12px; margin: 3px 6px; padding: 10px 20px; }
        QTabBar::tab:selected { background: rgba(255,255,255,14); border: 1px solid rgba(255,255,255,22); }
        QPushButton#shellDialogBack { padding: 10px 18px; border-radius: 14px; }
        QAbstractButton:focus { border-color: #eeeeee; }
        QComboBox { border-radius: 12px; background: #303030; padding: 7px 16px; }
        QComboBox::drop-down { border: none; width: 36px; }
        QComboBox::down-arrow { image: url(:/launcher/icons/chevron-down.png); width: 16px; height: 16px; }
        QCheckBox::indicator { width: 21px; height: 21px; border-radius: 6px; border: 1px solid #777777; background: #353535; }
        QCheckBox::indicator:checked { background: #eeeeee; image: url(:/launcher/icons/check-dark.png); }
    )") +
                       (tvMode ? QString(R"(
        QLabel,QCheckBox,QComboBox,QSpinBox,QDoubleSpinBox,QPushButton,QLineEdit,QGroupBox { font-size: 16px; }
        QComboBox,QSpinBox,QDoubleSpinBox,QLineEdit,QPushButton { min-height: 34px; padding: 7px 12px; }
        QTabBar::tab { font-size: 17px; padding: 16px 24px; }
        QCheckBox::indicator { width: 22px; height: 22px; }
        QSlider::handle:horizontal { width: 22px; margin: -8px 0; border-radius: 11px; }
        QSlider { min-height: 40px; }
    )")
                               : QString{}));
  dialog.ensurePolished();
  card->ensurePolished();
  if (card->layout())
    card->layout()->activate();
  placeShellDialog(&dialog);
  backdrop->setGeometry(dialog.rect());
  backdrop->lower();
  for (auto *box : dialog.findChildren<QDialogButtonBox *>())
    for (auto *b : box->buttons()) {
      const auto role = box->buttonRole(b);
      b->setIcon(
          shellIcon(role == QDialogButtonBox::RejectRole ? "x" : "check"));
    }
  if (!qobject_cast<QFileDialog *>(&dialog))
    for (auto *combo : dialog.findChildren<QComboBox *>())
      if (!combo->isEditable() &&
          !combo->property("shellStyledChoices").toBool()) {
        combo->setProperty("shellStyledChoices", true);
        combo->installEventFilter(new ChoiceEvents(combo));
      }
  // The overlay fades once; only the active panel repaints during the
  // transition.
  const bool fileBrowser = qobject_cast<QFileDialog *>(&dialog);
  if (!fileBrowser)
    shellAnimateButtons(&dialog);
  if (dialog.property("shellDrawer").toBool()) {
    if (dialog.property("shellNavigationPanel").toBool()) {
      auto *button = dialog.parentWidget()->findChild<QWidget *>("desktopMenu");
      const auto anchor = dialog.property("shellChoiceAnchor").toRect();
      shellRevealFrom(&dialog,
                      dialog.property("shellChoicePanel").toBool()
                          ? anchor.center()
                      : button ? button->mapToGlobal(button->rect().center())
                               : dialog.mapToGlobal(QPoint(20, 0)));
    } else if (!QSettings("GekkoAOT", "GekkoAOT")
                    .value("ui/reducedMotion", false)
                    .toBool()) {
      shellShadow(&dialog, 24);
      auto *motion = new QPropertyAnimation(&dialog, "pos", &dialog);
      motion->setObjectName("shellPositionMotion");
      motion->setDuration(260);
      const QPoint end = dialog.pos();
      motion->setStartValue(
          end +
          QPoint(dialog.property("shellDrawerLeft").toBool() ? -14 : 14, 4));
      motion->setEndValue(end);
      QEasingCurve spring(QEasingCurve::OutBack);
      spring.setOvershoot(.55);
      motion->setEasingCurve(spring);
      motion->start(QAbstractAnimation::DeleteWhenStopped);
    } else
      shellShadow(&dialog, 24);
  } else
    shellRevealSnapshot(&dialog);
  dialog.setProperty("shellPresented", true);
  dialog.setProperty("shellPresentedTV", tvMode);
}
void refreshShellValues(QWidget *scope) {
  for (auto *spin : scope->findChildren<QAbstractSpinBox *>())
    if (spin->property("shellStepper").toBool())
      if (auto *label =
              spin->parentWidget()->findChild<QLabel *>("shellStepperValue"))
        label->setText(spin->text());
}
QStringList shellChooseFiles(QWidget *parent, const QString &title,
                             const QString &directory, bool multiple,
                             bool folders) {
  QFileDialog dialog(parent, title, directory);
  dialog.setOption(QFileDialog::DontUseNativeDialog);
  dialog.setFileMode(folders ? QFileDialog::Directory
                             : (multiple ? QFileDialog::ExistingFiles
                                         : QFileDialog::ExistingFile));
  if (folders)
    dialog.setOption(QFileDialog::ShowDirsOnly);
  else
    dialog.setNameFilter(
        "Game sources (*.iso *.gcm *.rvz *.wia *.gcz *.wbfs *.dol)");
  presentShellDialog(dialog, title, shellTVMode(parent));
  return execShellDialog(dialog) == QDialog::Accepted ? dialog.selectedFiles()
                                                      : QStringList{};
}
#include <QStringList>
static void initializeShellAssets() {
  static const bool ready = [] {
    Q_INIT_RESOURCE(launcher_assets);
    return true;
  }();
  Q_UNUSED(ready);
}
QColor bannerAccent(const QImage &image) {
  if (image.isNull())
    return QColor("#575757");
  const auto sample =
      image.scaled(32, 12, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  double r = 0, g = 0, b = 0, total = 0;
  for (int y = 0; y < sample.height(); ++y)
    for (int x = 0; x < sample.width(); ++x) {
      const auto c = sample.pixelColor(x, y);
      const double weight = c.hsvSaturationF() * c.valueF() * c.alphaF();
      if (weight < .08)
        continue;
      r += c.redF() * weight;
      g += c.greenF() * weight;
      b += c.blueF() * weight;
      total += weight;
    }
  if (total < .01)
    return QColor("#575757");
  QColor color = QColor::fromRgbF(r / total, g / total, b / total);
  color.setHsvF(color.hsvHueF(), qMin(.48, color.hsvSaturationF()), .30);
  return color;
}
QIcon shellIcon(const QString &name, bool dark) {
  initializeShellAssets();
  static QMap<QString, QIcon> cache;
  const QString key = name + (dark ? "-dark" : "-light");
  if (cache.contains(key))
    return cache.value(key);
  QPixmap source(":/launcher/icons/" + name + ".png");
  if (source.isNull())
    return {};
  auto tint = [&](const QColor &color) {
    QPixmap result = source;
    QPainter painter(&result);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(result.rect(), color);
    return result;
  };
  QIcon icon;
  icon.addPixmap(tint(QColor(dark ? "#212121" : "#e9e9e9")), QIcon::Normal);
  icon.addPixmap(tint(QColor("#797979")), QIcon::Disabled);
  icon.addPixmap(tint(QColor(dark ? "#212121" : "#ffffff")), QIcon::Active);
  cache.insert(key, icon);
  return icon;
}
QFont shellDisplayFont(int pixels) {
  initializeShellAssets();
  static const QString family = [] {
    const int id =
        QFontDatabase::addApplicationFont(":/launcher/fonts/Doto-Variable.ttf");
    const auto families = QFontDatabase::applicationFontFamilies(id);
    return families.isEmpty() ? QStringLiteral("monospace") : families.first();
  }();
  QFont font(family);
  font.setPixelSize(pixels);
  font.setWeight(QFont::Bold);
  font.setLetterSpacing(QFont::AbsoluteSpacing, 0.5);
  return font;
}
QFont shellBodyFont(int pixels) {
  initializeShellAssets();
  static const QString family = [] {
    const int id = QFontDatabase::addApplicationFont(
        ":/launcher/fonts/GoogleSansFlex-Variable.ttf");
    const auto families = QFontDatabase::applicationFontFamilies(id);
    return families.isEmpty() ? QStringLiteral("sans-serif") : families.first();
  }();
  QFont font(family);
  font.setPixelSize(pixels);
  font.setWeight(QFont::Medium);
  return font;
}
QString shellControlsStyle() {
  return QStringLiteral(R"(
        QWidget#livingRoom { color: #f4f4f4; }
        QLabel { background: transparent; border: none; color: #f3f3f3; }
        QLabel[muted="true"] { color: #b7b7b7; }
        QFrame#glassNavigation, QFrame#glassSettings {
            background: rgba(62,62,62,155); border: 1px solid rgba(255,255,255,30); border-radius: 24px;
        }
        QFrame#glassSettings { background: #282828; border-radius: 22px; }
        QPushButton { background: transparent; color: #d5d5d5; border: 1px solid transparent;
            border-radius: 18px; padding: 12px 20px; font-size: 15px; }
        QPushButton:hover { background: rgba(255,255,255,14); color: white; }
        QPushButton:checked { background: rgba(255,255,255,18); color: white; }
        QPushButton:focus { border: 2px solid #f4f4f4; color: white; background: rgba(255,255,255,25); }
        QPushButton:disabled { color: #797979; }
        QPushButton#shellPlay { background: #eeeeee; color: #212121; padding: 15px 32px;
            border-radius: 22px; font-weight: 600; font-size: 17px; }
        QPushButton#shellPlay:hover { background: white; }
        QPushButton#shellPlay:focus { border: 2px solid white; background: white; }
        QPushButton#shellCompile, QPushButton#shellQueue { background: rgba(100,100,104,55);
            border: 1px solid rgba(255,255,255,22); }
        QPushButton#shellCompile:focus, QPushButton#shellQueue:focus { border-color: white; }
        QLineEdit { background: rgba(62,62,62,120); border: 1px solid rgba(255,255,255,25);
            border-radius: 18px; color: white; padding: 12px 18px; font-size: 14px; }
        QLineEdit:focus { border-color: #eeeeee; }
        QScrollArea { background: transparent; border: none; }
        QScrollArea > QWidget > QWidget { background: transparent; }
    )");
}

QIcon shellProjectIcon() {
  initializeShellAssets();
  const QString custom =
      QSettings("GekkoAOT", "GekkoAOT").value("ui/projectLogo").toString();
  QIcon icon(custom.isEmpty() ? QString(":/launcher/app-icon.png") : custom);
  return icon.isNull() ? QIcon(":/launcher/app-icon.png") : icon;
}
QIcon shellGameIcon(const QImage &banner) {
  if (banner.isNull())
    return shellProjectIcon();
  QPixmap canvas(256, 256);
  canvas.fill(Qt::transparent);
  QPainter p(&canvas);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  QLinearGradient glass(0, 0, 256, 256);
  glass.setColorAt(0, bannerAccent(banner).lighter(170));
  glass.setColorAt(1, QColor("#242424"));
  p.setBrush(glass);
  p.setPen(QColor(255, 255, 255, 60));
  p.drawRoundedRect(QRectF(4, 4, 248, 248), 48, 48);
  const QSize size = banner.size().scaled(224, 168, Qt::KeepAspectRatio);
  p.drawImage(
      QRect(QPoint((256 - size.width()) / 2, (256 - size.height()) / 2), size),
      banner);
  p.end();
  return QIcon(canvas);
}
void shellShadow(QWidget *widget, int radius) {
  if (widget->graphicsEffect())
    return;
  auto *shadow = new QGraphicsDropShadowEffect(widget);
  shadow->setBlurRadius(radius);
  shadow->setOffset(0, 3);
  shadow->setColor(QColor(0, 0, 0, 35));
  widget->setGraphicsEffect(shadow);
}
void shellAnimateButtons(QWidget *scope) {
  for (auto *button : scope->findChildren<QAbstractButton *>()) {
    if (button->property("shellButtonMotion").toBool() ||
        button->icon().isNull())
      continue;
    button->setProperty("shellButtonMotion", true);
    const QSize normal = button->iconSize();
    auto *motion = new QPropertyAnimation(button, "iconSize", button);
    QObject::connect(button, &QAbstractButton::pressed, button,
                     [button, motion, normal] {
                       if (QSettings("GekkoAOT", "GekkoAOT")
                               .value("ui/reducedMotion", false)
                               .toBool())
                         return;
                       motion->stop();
                       button->setIconSize(normal * .9);
                     });
    QObject::connect(button, &QAbstractButton::released, button,
                     [button, motion, normal] {
                       motion->stop();
                       if (QSettings("GekkoAOT", "GekkoAOT")
                               .value("ui/reducedMotion", false)
                               .toBool()) {
                         button->setIconSize(normal);
                         return;
                       }
                       motion->setDuration(190);
                       motion->setStartValue(button->iconSize());
                       motion->setEndValue(normal);
                       QEasingCurve spring(QEasingCurve::OutBack);
                       spring.setOvershoot(.5);
                       motion->setEasingCurve(spring);
                       motion->start();
                     });
  }
}
void shellReveal(QWidget *widget) { shellRevealSnapshot(widget); }

namespace {
// Transform the rendered popover, keeping its layout and hit targets stable.
// The pivot is the actual invoking button, including when it sits above the
// popup. A small painted shadow shares the same transform as its content.
class PopoverEffect final : public QGraphicsEffect {
public:
  QPointF origin;
  qreal progress = 1;
  bool snapshot = false;
  qreal initialScale = .78;
  QPointer<QVariantAnimation> pendingMotion;
  bool motionStarted = false;
  QPixmap cached;
  QPoint cachedOffset;
  explicit PopoverEffect(QObject *parent) : QGraphicsEffect(parent) {}

protected:
  QRectF boundingRectFor(const QRectF &rect) const override {
    return rect.adjusted(-20, -20, 20, 24);
  }
  void draw(QPainter *painter) override {
    QPoint offset;
    QPixmap source;
    if (snapshot && !cached.isNull()) {
      source = cached;
      offset = cachedOffset;
    } else {
      source =
          sourcePixmap(Qt::LogicalCoordinates, &offset, QGraphicsEffect::NoPad);
      if (snapshot) {
        cached = source;
        cachedOffset = offset;
      }
    }
    const qreal scale = initialScale + (1 - initialScale) * progress;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setOpacity(qBound(0.0, .15 + .85 * progress, 1.0));
    painter->translate(origin);
    painter->scale(scale, scale);
    painter->translate(-origin);
    const QRectF box = sourceBoundingRect(Qt::LogicalCoordinates);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 3));
    for (int spread = snapshot ? 0 : 15; spread > 0; --spread)
      painter->drawRoundedRect(
          box.adjusted(-spread, -spread + 4, spread, spread + 4), 22 + spread,
          22 + spread);
    painter->drawPixmap(offset, source);
    painter->restore();
    if (pendingMotion && !motionStarted) {
      motionStarted = true;
      QTimer::singleShot(0, pendingMotion, [motion = pendingMotion] {
        if (motion)
          motion->start(QAbstractAnimation::DeleteWhenStopped);
      });
    }
  }
};
} // namespace
void shellRevealFrom(QWidget *widget, const QPoint &globalOrigin) {
  auto *effect = new PopoverEffect(widget);
  effect->origin = widget->mapFromGlobal(globalOrigin);
  widget->setProperty("shellRevealOrigin", effect->origin.toPoint());
  widget->setGraphicsEffect(effect);
  if (widget->property("shellReducedMotion").toBool() ||
      QSettings("GekkoAOT", "GekkoAOT")
          .value("ui/reducedMotion", false)
          .toBool())
    return;
  effect->progress = 0;
  auto *motion = new QVariantAnimation(effect);
  motion->setDuration(230);
  motion->setStartValue(0.0);
  motion->setEndValue(1.0);
  QEasingCurve spring(QEasingCurve::OutBack);
  spring.setOvershoot(.5);
  motion->setEasingCurve(spring);
  QObject::connect(motion, &QVariantAnimation::valueChanged, effect,
                   [effect](const QVariant &value) {
                     effect->progress = value.toReal();
                     effect->update();
                   });
  effect->pendingMotion = motion;
  effect->update();
}

void shellRevealSnapshot(QWidget *widget) {
  widget->setProperty("shellSnapshotTransition", true);
  if (QSettings("GekkoAOT", "GekkoAOT")
          .value("ui/reducedMotion", false)
          .toBool())
    return;
  auto *effect = new PopoverEffect(widget);
  effect->snapshot = true;
  effect->initialScale =
      widget->property("shellSceneTransition").toBool() ? .94 : .97;
  effect->origin = widget->rect().center();
  effect->progress = 0;
  widget->setGraphicsEffect(effect);
  auto *motion = new QVariantAnimation(effect);
  motion->setDuration(widget->property("shellSceneTransition").toBool() ? 300
                                                                        : 180);
  motion->setStartValue(0.0);
  motion->setEndValue(1.0);
  motion->setEasingCurve(QEasingCurve::OutCubic);
  QObject::connect(motion, &QVariantAnimation::valueChanged, effect,
                   [effect](const QVariant &value) {
                     effect->progress = value.toReal();
                     effect->update();
                   });
  QObject::connect(
      motion, &QVariantAnimation::finished, widget, [widget, effect] {
        QTimer::singleShot(0, widget,
                           [widget, guard = QPointer<PopoverEffect>(effect)] {
                             if (guard && widget->graphicsEffect() == guard)
                               widget->setGraphicsEffect(nullptr);
                           });
      });
  effect->pendingMotion = motion;
  effect->update();
}

namespace {
class ExitSnapshot final : public QWidget {
public:
  QPixmap picture;
  QPointF origin;
  qreal progress = 1;
  bool popover = false;
  explicit ExitSnapshot(QWidget *host) : QWidget(host) {
    setObjectName("shellExitTransition");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal scale = popover ? .88 + .12 * progress : .97 + .03 * progress;
    p.setOpacity(progress);
    p.translate(origin);
    p.scale(scale, scale);
    p.translate(-origin);
    p.drawPixmap(0, 0, picture);
  }
};
} // namespace
void shellRetire(QWidget *widget) {
  auto *host =
      widget->parentWidget() ? widget->parentWidget()->window() : nullptr;
  if (!host || !host->isVisible() ||
      widget->property("shellExitCaptured").toBool() ||
      widget->property("shellSuppressExit").toBool() ||
      QCoreApplication::closingDown() ||
      QSettings("GekkoAOT", "GekkoAOT")
          .value("ui/reducedMotion", false)
          .toBool())
    return;
  widget->setProperty("shellExitCaptured", true);
  auto *snapshot = new ExitSnapshot(host);
  snapshot->picture = widget->grab();
  snapshot->setGeometry(QRect(
      host->mapFromGlobal(widget->mapToGlobal(QPoint{})), widget->size()));
  snapshot->popover = widget->property("shellNavigationPanel").toBool() ||
                      widget->objectName() == "shellContextMenu" ||
                      widget->objectName() == "glassSettings";
  snapshot->origin = widget->property("shellRevealOrigin").isValid()
                         ? widget->property("shellRevealOrigin").toPoint()
                         : widget->rect().center();
  snapshot->show();
  snapshot->raise();
  auto *motion = new QVariantAnimation(snapshot);
  motion->setDuration(150);
  motion->setStartValue(1.0);
  motion->setEndValue(0.0);
  motion->setEasingCurve(QEasingCurve::InCubic);
  QObject::connect(motion, &QVariantAnimation::valueChanged, snapshot,
                   [snapshot](const QVariant &value) {
                     snapshot->progress = value.toReal();
                     snapshot->update();
                   });
  QObject::connect(motion, &QVariantAnimation::finished, snapshot,
                   &QObject::deleteLater);
  motion->start();
}

void showShellChoices(QComboBox *combo) {
  if (!combo || !combo->count())
    return;
  QDialog picker(combo->window());
  picker.setObjectName("shellChoicePicker");
  picker.setProperty("shellChoicePanel", true);
  picker.setProperty("shellChoiceAnchor",
                     QRect(combo->mapToGlobal(QPoint{}), combo->size()));
  const bool cards = combo->property("shellCardChoices").toBool();
  auto *layout = new QVBoxLayout(&picker);
  layout->setContentsMargins(2, 2, 2, 2);
  layout->setSpacing(8);
  auto *list = new QListWidget;
  list->setObjectName("shellChoiceList");
  list->setTextElideMode(Qt::ElideRight);
  list->setIconSize({24, 24});
  list->setStyleSheet(
      "QListWidget {background:transparent;border:none;outline:0;padding:3px;} "
      "QListWidget::item {padding:8px 12px;margin:2px 3px;border:1px solid "
      "transparent;border-radius:10px;} QListWidget::item:selected "
      "{background:rgba(255,255,255,12);border-color:rgba(255,255,255,45);} "
      "QListWidget::item:hover {background:rgba(255,255,255,8);}");
  if (combo->count() > 10) {
    auto *search = new QLineEdit;
    search->setObjectName("shellChoiceSearch");
    search->setPlaceholderText(cards ? "Search memory cards…" : "Search…");
    search->addAction(shellIcon("search"), QLineEdit::LeadingPosition);
    layout->addWidget(search);
    QObject::connect(
        search, &QLineEdit::textChanged, &picker, [list](const QString &query) {
          for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(
                !list->item(i)->text().contains(query, Qt::CaseInsensitive));
          if (list->currentItem() && list->currentItem()->isHidden())
            for (int i = 0; i < list->count(); ++i)
              if (!list->item(i)->isHidden()) {
                list->setCurrentRow(i);
                break;
              }
        });
  }
  for (int i = 0; i < combo->count(); ++i) {
    QString text = combo->itemText(i);
    if (cards)
      text.replace(" / ", "\n");
    auto *item = new QListWidgetItem(
        cards ? shellIcon("memory-stick") : combo->itemIcon(i), text, list);
    item->setSizeHint({0, cards ? 64 : 44});
    item->setToolTip(cards ? combo->itemData(i).toString()
                           : combo->itemText(i));
    item->setData(Qt::UserRole, i);
  }
  list->setFixedHeight(
      qMin(cards ? 360 : 308, combo->count() * (cards ? 68 : 48) + 10));
  layout->addWidget(list);
  list->setCurrentRow(combo->currentIndex());
  auto choose = [&](QListWidgetItem *) {
    if (list->currentItem() && !list->currentItem()->isHidden())
      picker.accept();
  };
  QObject::connect(list, &QListWidget::itemClicked, &picker, choose);
  QObject::connect(list, &QListWidget::itemActivated, &picker, choose);
  presentShellDialog(picker,
                     combo->property("shellChoiceTitle").toString().isEmpty()
                         ? "Choose an option"
                         : combo->property("shellChoiceTitle").toString(),
                     shellTVMode(combo));
  QTimer::singleShot(0, &picker, [list] {
    list->setFocus();
    list->scrollToItem(list->currentItem());
  });
  if (execShellDialog(picker) == QDialog::Accepted && list->currentItem()) {
    const int index = list->currentItem()->data(Qt::UserRole).toInt();
    combo->setCurrentIndex(index);
    QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection,
                              Q_ARG(int, index));
  }
}

QDialog *shellActiveDialog(QWidget *owner) {
  const auto dialogs = owner->findChildren<QDialog *>();
  for (auto it = dialogs.crbegin(); it != dialogs.crend(); ++it)
    if ((*it)->isVisible() && (*it)->property("shellPresented").toBool())
      return *it;
  return nullptr;
}

int execShellDialog(QDialog &dialog) {
  if (dialog.isWindow())
    return dialog.exec();
  // QDialog::exec would make the parent native window modal. A child overlay
  // shares that window, so Qt would reject mouse/keyboard events for both.
  // The full-size child already blocks the content underneath geometrically.
  QEventLoop loop;
  QObject::connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
  dialog.setModal(false);
  dialog.setAttribute(Qt::WA_ShowModal, false);
  dialog.show();
  dialog.raise();
  QTimer::singleShot(0, &dialog, [&dialog] {
    if (auto *tree = dialog.findChild<QTreeView *>("treeView");
        qobject_cast<QFileDialog *>(&dialog) && tree)
      tree->setFocus();
    else if (auto *back = dialog.findChild<QPushButton *>("shellDialogBack");
             back && back->isVisible())
      back->setFocus();
  });
  loop.exec();
  return dialog.result();
}

QWidget *shellDotGamepad(QWidget *parent) {
  class DotPad final : public QWidget {
  public:
    explicit DotPad(QWidget *p) : QWidget(p) {
      setFixedSize(92, 42);
      setAccessibleName("GameCube controller");
    }

  protected:
    void paintEvent(QPaintEvent *) override {
      static const char *shape[] = {
          "     ####       ####     ", "   ###################   ",
          "  ####  #########  ####  ", " ####    #######    #### ",
          " ####  ###########  #### ", " ####################### ",
          " #######   ###   ####### ", " ######     #     ###### ",
          " #####             ##### ", " ####               #### ",
          "  ##                 ##  "};
      QPainter p(this);
      p.setRenderHint(QPainter::Antialiasing);
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(210, 210, 210, 180));
      for (int y = 0; y < 11; ++y)
        for (int x = 0; shape[y][x]; ++x)
          if (shape[y][x] == '#')
            p.drawEllipse(QPointF(3 + x * 3.5, 3 + y * 3.5), .9, .9);
    }
  };
  return new DotPad(parent);
}
QWidget *shellDotDisc(QWidget *parent) {
  class DotDisc final : public QWidget {
  public:
    explicit DotDisc(QWidget *p) : QWidget(p) {
      setFixedSize(48, 48);
      setAccessibleName("Game disc");
    }

  protected:
    void paintEvent(QPaintEvent *) override {
      QPainter painter(this);
      painter.setRenderHint(QPainter::Antialiasing);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(210, 210, 210, 155));
      for (const double radius : {5.0, 8.5, 12.0, 15.5, 19.0}) {
        const int count = qRound(2 * 3.141592653589793 * radius / 3.3);
        for (int i = 0; i < count; ++i) {
          const double angle = 2 * 3.141592653589793 * (i + .5) / count;
          painter.drawEllipse(QPointF(24 + radius * std::cos(angle),
                                      24 + radius * std::sin(angle)),
                              .7, .7);
        }
      }
    }
  };
  return new DotDisc(parent);
}
