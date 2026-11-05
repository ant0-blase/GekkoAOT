#pragma once
#include <QString>
#include <QPalette>
#include <QColor>

inline QPalette launcherPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#2b2b2b"));
    palette.setColor(QPalette::WindowText, QColor("#e8e8e8"));
    palette.setColor(QPalette::Base, QColor("#252525"));
    palette.setColor(QPalette::AlternateBase, QColor("#303030"));
    palette.setColor(QPalette::Text, QColor("#e8e8e8"));
    palette.setColor(QPalette::Button, QColor("#3a3a3a"));
    palette.setColor(QPalette::ButtonText, QColor("#ececec"));
    palette.setColor(QPalette::Highlight, QColor("#565656"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::ToolTipBase, QColor("#3a3a3a"));
    palette.setColor(QPalette::ToolTipText, QColor("#eeeeee"));
    palette.setColor(QPalette::PlaceholderText, QColor("#929292"));
    palette.setColor(QPalette::Light, QColor("#555555"));
    palette.setColor(QPalette::Midlight, QColor("#444444"));
    palette.setColor(QPalette::Mid, QColor("#484848"));
    palette.setColor(QPalette::Dark, QColor("#202020"));
    palette.setColor(QPalette::Shadow, QColor("#151515"));
    palette.setColor(QPalette::BrightText, Qt::white);
    palette.setColor(QPalette::Link, QColor("#b9d5ee"));
    palette.setColor(QPalette::LinkVisited, QColor("#b8c8d7"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor("#858585"));
    return palette;
}

inline QString launcherChromeStyleSheet()
{
    return QStringLiteral(R"QSS(
        QScrollBar:vertical { background: #252525; width: 12px; margin: 0; border: none; }
        QScrollBar:horizontal { background: #252525; height: 12px; margin: 0; border: none; }
        QScrollBar::handle { background: #626262; border: 2px solid #252525; border-radius: 5px; }
        QScrollBar::handle:vertical { min-height: 28px; }
        QScrollBar::handle:horizontal { min-width: 28px; }
        QScrollBar::handle:hover { background: #7b7b7b; }
        QScrollBar::handle:pressed { background: #999999; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: none; background: none; }
        QScrollBar::add-page, QScrollBar::sub-page { background: none; }
        QAbstractScrollArea::corner { background: #252525; border: none; }
        QToolBar::separator { background: #4a4a4a; width: 1px; height: 1px; margin: 8px 5px; }
        QMainWindow::separator { background: #333333; width: 5px; height: 5px; }
        QMainWindow::separator:hover { background: #606060; }
        QSplitter::handle { background: #383838; }
        QDockWidget { color: #e8e8e8; }
        QDockWidget::title { background: #303030; color: #e8e8e8; padding: 8px 10px; }
    )QSS");
}

// Shared by the launcher's settings windows; keep the launcher grey palette.
inline QString settingsStyleSheet()
{
    return QStringLiteral(R"QSS(
        QDialog { background: #2b2b2b; color: #e8e8e8; }
        QLabel, QCheckBox, QGroupBox { color: #e8e8e8; font-size: 13px; }
        QLabel[muted="true"] { color: #aaaaaa; }
        QScrollArea, QScrollArea > QWidget > QWidget { background: #2f2f2f; border: none; }
        QTabWidget::pane { border: 1px solid #474747; border-radius: 20px; background: #2f2f2f; }
        QTabBar::tab { color: #aaaaaa; background: transparent; padding: 11px 22px; margin-right: 4px; border-bottom: 3px solid transparent; }
        QTabBar::tab:selected { color: #ffffff; border-bottom: 3px solid #bdbdbd; }
        QTabBar::tab:hover { color: #ffffff; background: #383838; }
        QSlider { min-height: 30px; }
        QSlider::groove:horizontal { height: 6px; background: #4a4a4a; border-radius: 3px; }
        QSlider::sub-page:horizontal { background: #a0a0a0; border-radius: 3px; }
        QSlider::handle:horizontal { background: #ededed; width: 16px; margin: -5px 0; border-radius: 8px; }
        QSlider::handle:horizontal:hover { background: #ffffff; }
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QKeySequenceEdit {
            min-height: 28px; background: #262626; color: #ececec;
            border: 1px solid #565656; border-radius: 14px; padding: 6px 10px;
        }
        QKeySequenceEdit QLineEdit { border: none; padding: 0; min-height: 24px; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QKeySequenceEdit:focus { border-color: #bdbdbd; }
        QComboBox QAbstractItemView { background: #2f2f2f; color: #ececec; selection-background-color: #505050; }
        QPushButton { color: #ededed; background: #3a3a3a; border: 1px solid #565656; border-radius: 14px; min-height: 30px; padding: 6px 14px; }
        QPushButton:hover { background: #454545; border-color: #777777; }
        QPushButton:pressed { background: #303030; }
        QPushButton:default { background: #4a4a4a; border-color: #999999; }
        QPushButton:disabled { color: #888888; background: #303030; border-color: #444444; }
        QPushButton#DangerButton { color: #ffb6b6; }
        QGroupBox { background: #313131; border: 1px solid #474747; border-radius: 18px; margin-top: 12px; padding-top: 10px; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }
        QPlainTextEdit { background: #1c1c1c; color: #e2e2e2; border: 1px solid #474747; border-radius:14px;padding:10px; }
        QToolTip { background: #3a3a3a; color: #eeeeee; border: 1px solid #666666; }
    )QSS") + launcherChromeStyleSheet();
}
