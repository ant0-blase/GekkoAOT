// SPDX-License-Identifier: GPL-3.0-or-later
// Private to the MainWindow implementation (MainWindow*.cpp): shared includes,
// constants and stateless helpers. The anonymous namespace gives every
// translation unit its own copy.
#pragma once
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "MainWindow.hpp"
#include <cmath>
#include "MemoryCardManager.hpp"
#include "ModManagerDialog.hpp"
#include "GraphicsConfigDialog.hpp"
#include "../runtime/gx/graphics_presets.h"
#include "SettingsStyle.hpp"
#include "DiscMetadata.hpp"
#include "GameLibrary.hpp"
#include "GameCard.hpp"
#include "CompilationSidebar.hpp"
#include "LivingRoomView.hpp"
#include "ShellGamepad.hpp"
#include "ShellStyle.hpp"
#include "ShellKeyboard.hpp"
#include "ShellNavigation.hpp"
#include "ShellTitleBar.hpp"
#include "BuildProgress.hpp"
#include <QCryptographicHash>
#include "InputProfile.hpp"
#include "InputConfigDialog.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAbstractSlider>
#include <QAbstractItemView>
#include <QKeyEvent>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QDragEnterEvent>
#include <QDateTime>
#include <QDropEvent>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QScrollArea>
#include <QScreen>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSettings>
#include <QSize>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSlider>
#include <QStringList>
#include <QStackedLayout>
#include <QStatusBar>
#include <QTabWidget>
#include <QStyle>
#include <QTextCursor>
#include <QTextStream>
#include <QThread>
#include <QGuiApplication>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#ifndef GEKKOAOT_VERSION
#define GEKKOAOT_VERSION "dev"
#endif

#ifndef GEKKOAOT_SOURCE_DIR
#define GEKKOAOT_SOURCE_DIR "."
#endif

namespace
{
constexpr const char* kDolRecomp = "71ce7f9";
constexpr const char* kAurora = "7f2801c";
constexpr const char* kNod = "2.0.0-a12";

QLabel* g_perfHud = nullptr;

QString runtimeResourceRoot()
{
    const QString explicitRoot = qEnvironmentVariable("GEKKOAOT_RESOURCE_DIR").trimmed();
    if (!explicitRoot.isEmpty())
        return QDir::cleanPath(explicitRoot);

    const QDir compiled(QString::fromUtf8(GEKKOAOT_SOURCE_DIR));
    if (QFileInfo::exists(compiled.filePath("runtime")) &&
        QFileInfo::exists(compiled.filePath("game-packs")))
        return compiled.absolutePath();

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString installed = QDir::cleanPath(appDir.filePath("../share/gekkoaot"));
    if (QFileInfo::exists(QDir(installed).filePath("runtime")) &&
        QFileInfo::exists(QDir(installed).filePath("game-packs")))
        return installed;

    const QDir cwd(QDir::currentPath());
    if (QFileInfo::exists(cwd.filePath("runtime")) &&
        QFileInfo::exists(cwd.filePath("game-packs")))
        return cwd.absolutePath();

    return compiled.absolutePath();
}

QString runtimeStateRoot()
{
    const QString explicitState = qEnvironmentVariable("GEKKOAOT_STATE_DIR").trimmed();
    if (!explicitState.isEmpty())
        return QDir::cleanPath(explicitState);

    const QString resourceRoot = runtimeResourceRoot();
    if (QFileInfo::exists(QDir(resourceRoot).filePath("CMakeLists.txt")))
        return QDir(resourceRoot).filePath(".gekkoaot");

#ifdef Q_OS_WIN
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA").trimmed();
    if (!localAppData.isEmpty())
        return QDir(localAppData).filePath("GekkoAOT");
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return appData.isEmpty() ? QDir::temp().filePath("GekkoAOT") : appData;
#else
    QString base = qEnvironmentVariable("XDG_CACHE_HOME").trimmed();
    if (base.isEmpty())
        base = QDir::home().filePath(".cache");
    return QDir(base).filePath("gekkoaot");
#endif
}

class NativeRenderWidget final : public QWidget
{
public:
    explicit NativeRenderWidget(QWidget* parent = nullptr) : QWidget(parent) {}

    // Qt documents this as the companion to WA_PaintOnScreen for widgets whose
    // native surface is painted by an external renderer. Returning nullptr
    // prevents QWidget from repeatedly asking for a raster paint engine while
    // Aurora owns the X11/Vulkan presentation surface.
    QPaintEngine* paintEngine() const override { return nullptr; }
};

class SessionWindow final : public QMainWindow
{
public:
    explicit SessionWindow(QWidget* parent):QMainWindow(parent,Qt::Window){}
    std::function<void()> closing;
protected:
    void closeEvent(QCloseEvent* e) override {if(closing)closing();e->accept();}
};

constexpr QFileDialog::Options kPortableFileDialogOptions = QFileDialog::DontUseNativeDialog;

QIcon themedIcon(QWidget* widget, const QString& name, QStyle::StandardPixmap fallback)
{
    static const QMap<QString,QString> icons = {
        {"document-open","folder-open"},{"media-playback-start","play"},{"media-playback-stop","square"},
        {"media-playback-pause","pause"},{"view-fullscreen","maximize"},{"video-display","tv-minimal"},
        {"preferences-desktop-display","sliders-horizontal"},{"input-gaming","gamepad-2"},
        {"preferences-system","settings-2"},{"view-refresh","rotate-cw"},{"edit-find","scan-search"},
        {"system-run","cpu"},{"edit-delete","trash"},{"applications-games","sparkles"},
        {"view-list-details","list-video"},{"media-optical","disc-3"}};
    if (icons.contains(name)) {
        const auto icon = shellIcon(icons.value(name));
        if (!icon.isNull()) return icon;
    }
    QIcon icon = QIcon::fromTheme(name);
    if (icon.isNull())
        icon = widget->style()->standardIcon(fallback);
    return icon;
}

QLabel* mutedLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("muted", true);
    return label;
}


QString gamePackPathForId(const QString& discId)
{
    if (discId.isEmpty())
        return {};
    return QDir(runtimeResourceRoot()).filePath("game-packs/v1/" + discId + ".json");
}

QJsonObject loadGamePackJson(const QString& discId)
{
    QFile file(gamePackPathForId(discId));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {};
    const QJsonObject object = document.object();
    if (object.value("disc_id").toString() != discId)
        return {};
    return object;
}

QString gameOptionSettingsKey(const QString& discId, const QString& optionId)
{
    return QStringLiteral("gameEnhancements/%1/%2").arg(discId, optionId);
}

QString defaultOptionValue(const QJsonObject& option)
{
    const QJsonValue value = option.value("default");
    if (value.isBool())
        return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    if (value.isDouble())
        return QString::number(value.toDouble(), 'g', 12);
    return value.toString();
}

QString storedGameOptionValue(const QString& discId, const QJsonObject& option)
{
    const QString optionId = option.value("id").toString();
    QSettings settings("GekkoAOT", "GekkoAOT");
    return settings.value(gameOptionSettingsKey(discId, optionId), defaultOptionValue(option)).toString();
}

void storeGameOptionValue(const QString& discId, const QJsonObject& option, const QString& value)
{
    QSettings settings("GekkoAOT", "GekkoAOT");
    settings.setValue(gameOptionSettingsKey(discId, option.value("id").toString()), value);
}

QString optionDisplayValue(const QJsonObject& option, const QString& value)
{
    if (value.isEmpty())
        return option.value("empty_label").toString(QStringLiteral("Original"));
    const QString suffix = option.value("suffix").toString();
    return value + suffix;
}

quint32 be32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size())
        return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData() + offset);
    return (static_cast<quint32>(p[0]) << 24) | (static_cast<quint32>(p[1]) << 16) |
           (static_cast<quint32>(p[2]) << 8) | static_cast<quint32>(p[3]);
}



}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
