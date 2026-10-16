#pragma once
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QPoint>
#include <QString>
#include <QStringList>
QFont shellDisplayFont(int pixels);
QFont shellBodyFont(int pixels = 14);
QIcon shellIcon(const QString &name, bool dark = false);
QColor bannerAccent(const QImage &image);
QString shellControlsStyle();
class QWidget;
class QDialog;
class QComboBox;
bool shellTVMode(const QWidget *widget);
void presentShellDialog(QDialog &dialog, const QString &heading, bool tvMode);
void refreshShellValues(QWidget *scope);
QStringList shellChooseFiles(QWidget *parent, const QString &title,
                             const QString &directory = {},
                             bool multiple = false, bool folders = false);

QIcon shellProjectIcon();
QIcon shellGameIcon(const QImage &banner);
void shellShadow(QWidget *widget, int radius = 24);
void shellReveal(QWidget *widget);
void shellRevealSnapshot(QWidget *widget);
void shellRetire(QWidget *widget);
void showShellChoices(QComboBox *combo);
void shellRevealFrom(QWidget *widget, const QPoint &globalOrigin);
void shellAnimateButtons(QWidget *scope);

QDialog *shellActiveDialog(QWidget *owner);

int execShellDialog(QDialog &dialog);

QWidget *shellDotGamepad(QWidget *parent = nullptr);
QWidget *shellDotDisc(QWidget *parent = nullptr);

void setShellWindowIdentity(QWidget *window, const QString &suffix,
                            const QString &title, const QIcon &icon);
