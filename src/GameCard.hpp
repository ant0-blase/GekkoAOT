#pragma once
#include "GameLibrary.hpp"
#include "ShellStyle.hpp"
#include <QAbstractButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVariantAnimation>
#include <functional>
class GameCard final : public QAbstractButton {
public:
  LibraryEntry game;
  std::function<void()> highlighted;
  std::function<void()> doubleClicked;
  explicit GameCard(const LibraryEntry &entry, QWidget *parent)
      : QAbstractButton(parent), game(entry) {
    setObjectName("shellGameCard");
    setAccessibleName(entry.title);
    setToolTip(entry.title);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    m_image = entry.banner;

    m_motion.setDuration(230);
    QEasingCurve spring(QEasingCurve::OutBack);
    spring.setOvershoot(.5);
    m_motion.setEasingCurve(spring);
    QObject::connect(&m_motion, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &v) {
                       m_focus = v.toReal();
                       update();
                     });
  }
  void setActions(std::function<void()> action) {
    m_more = new QToolButton(this);
    m_more->setObjectName("gameCardActions");
    m_more->setIcon(shellIcon("menu"));
    m_more->setAccessibleName("Actions for " + game.title);
    m_more->setToolTip("Game actions");
    m_more->setFocusPolicy(Qt::ClickFocus);
    m_more->setIconSize({23, 23});
    m_more->hide();
    m_more->setStyleSheet(
        "QToolButton {background:transparent;border:1px solid "
        "transparent;border-radius:12px;padding:6px;}QToolButton:hover,"
        "QToolButton:focus "
        "{background:rgba(255,255,255,20);border-color:rgba(255,255,255,75);}");
    QObject::connect(m_more, &QToolButton::clicked, this,
                     [action] { action(); });
  }
  void setReducedMotion(bool value) {
    m_reduced = value;
    if (value) {
      m_motion.stop();
      m_focus = hasFocus() || underMouse();
      update();
    }
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QAbstractButton::resizeEvent(event);
    if (m_more)
      m_more->setGeometry(width() - 62, (height() - 42) / 2, 42, 42);
  }
  void mouseDoubleClickEvent(QMouseEvent *event) override {
    if (doubleClicked) {
      doubleClicked();
      event->accept();
    } else
      QAbstractButton::mouseDoubleClickEvent(event);
  }
  void focusInEvent(QFocusEvent *e) override {
    QAbstractButton::focusInEvent(e);
    animate();
    if (highlighted)
      highlighted();
  }
  void focusOutEvent(QFocusEvent *e) override {
    QAbstractButton::focusOutEvent(e);
    animate();
  }
  void enterEvent(QEnterEvent *e) override {
    QAbstractButton::enterEvent(e);
    animate();
  }
  void leaveEvent(QEvent *e) override {
    QAbstractButton::leaveEvent(e);
    animate();
  }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF r(12 - m_focus * 3, 14 - m_focus * 5,
                   width() - 24 + m_focus * 6, height() - 26 + m_focus * 6);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 20));
    p.drawRoundedRect(r.translated(0, 3), 20, 20);
    QLinearGradient surface(r.topLeft(), r.bottomRight());
    surface.setColorAt(0, QColor(76, 76, 76, 220));
    surface.setColorAt(1, QColor(40, 40, 40, 240));
    p.setBrush(surface);
    p.drawRoundedRect(r, 20, 20);
    QPainterPath clip;
    clip.addRoundedRect(r.adjusted(1, 1, -1, -1), 19, 19);
    p.save();
    p.setClipPath(clip);
    const QRectF picture(r.x(), r.y(), r.width(), r.height() - 58);
    QLinearGradient tint(picture.topLeft(), picture.bottomRight());
    tint.setColorAt(0, bannerAccent(m_image));
    tint.setColorAt(1, QColor(35, 35, 35, 80));
    p.fillRect(picture, tint);
    if (!m_image.isNull()) {
      const QSizeF size = QSizeF(m_image.size())
                              .scaled(QSizeF(qMin(224.0, picture.width() - 32),
                                             picture.height() - 28),
                                      Qt::KeepAspectRatio);
      p.drawImage(QRectF(picture.center() -
                             QPointF(size.width() / 2, size.height() / 2),
                         size),
                  m_image);
    }
    if (m_image.isNull()) {
      p.setFont(shellDisplayFont(34));
      p.setPen(QColor(160, 160, 160));
      p.drawText(picture, Qt::AlignCenter, game.title.left(2).toUpper());
    }
    QLinearGradient shade(picture.topLeft(), picture.bottomLeft());
    shade.setColorAt(.55, Qt::transparent);
    shade.setColorAt(1, QColor(25, 25, 25, 210));
    p.fillRect(picture, shade);
    p.restore();
    p.setPen(QColor(
        255, 255, 255,
        qBound(0, int(qMax(isChecked() ? 75.0 : 32.0, 32 + 205 * m_focus)),
               255)));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 20, 20);
    QFont f = font();
    f.setPixelSize(14);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(QColor(238, 238, 238));
    p.drawText(QRectF(r.x() + 16, r.bottom() - 48, r.width() - 32, 20),
               Qt::AlignVCenter,
               p.fontMetrics().elidedText(game.title, Qt::ElideRight,
                                          int(r.width() - 32)));
    f.setPixelSize(10);
    f.setWeight(QFont::Normal);
    p.setFont(f);
    p.setPen(QColor(164, 164, 164));
    p.drawText(QRectF(r.x() + 16, r.bottom() - 26, r.width() - 32, 14),
               Qt::AlignVCenter,
               game.gameId.isEmpty() ? "GAMECUBE"
                                     : "GAMECUBE  /  " + game.gameId);
  }

private:
  QToolButton *m_more = nullptr;
  QImage m_image, m_blurred;
  QVariantAnimation m_motion;
  qreal m_focus = 0;
  bool m_reduced = false;
  void animate() {
    if (m_more)
      m_more->setVisible(hasFocus() || underMouse() || m_more->hasFocus());
    const qreal target = hasFocus() || underMouse() ? 1 : 0;
    m_motion.stop();
    if (m_reduced) {
      m_focus = target;
      update();
      return;
    }
    m_motion.setStartValue(m_focus);
    m_motion.setEndValue(target);
    m_motion.start();
  }
};
