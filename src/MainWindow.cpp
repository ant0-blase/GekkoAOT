#include "MainWindow.hpp"
#include "InputProfile.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
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
#include <QPushButton>
#include <QRegularExpression>
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

constexpr QFileDialog::Options kPortableFileDialogOptions = QFileDialog::DontUseNativeDialog;

QIcon themedIcon(QWidget* widget, const QString& name, QStyle::StandardPixmap fallback)
{
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

int comboIndexForData(const QComboBox* combo, const QString& value)
{
    const int index = combo->findData(value);
    return index >= 0 ? index : 0;
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

int expand5(int value)
{
    return (value << 3) | (value >> 2);
}

int expand4(int value)
{
    return (value << 4) | value;
}

QImage decodeGameCubeBanner(const QByteArray& bnr)
{
    constexpr qsizetype textureOffset = 0x20;
    constexpr int width = 96;
    constexpr int height = 32;
    constexpr qsizetype textureBytes = width * height * 2;
    if (bnr.size() < textureOffset + textureBytes ||
        (bnr.left(4) != "BNR1" && bnr.left(4) != "BNR2"))
        return {};

    QImage image(width, height, QImage::Format_RGBA8888);
    const auto* src = reinterpret_cast<const unsigned char*>(bnr.constData() + textureOffset);
    qsizetype pos = 0;
    for (int tileY = 0; tileY < height; tileY += 4)
    {
        for (int tileX = 0; tileX < width; tileX += 4)
        {
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 4; ++x)
                {
                    const quint16 value = static_cast<quint16>(src[pos] << 8 | src[pos + 1]);
                    pos += 2;
                    int r, g, b, a;
                    if (value & 0x8000u)
                    {
                        a = 255;
                        r = expand5((value >> 10) & 0x1F);
                        g = expand5((value >> 5) & 0x1F);
                        b = expand5(value & 0x1F);
                    }
                    else
                    {
                        a = ((value >> 12) & 0x7) * 255 / 7;
                        r = expand4((value >> 8) & 0xF);
                        g = expand4((value >> 4) & 0xF);
                        b = expand4(value & 0xF);
                    }
                    image.setPixelColor(tileX + x, tileY + y, QColor(r, g, b, a));
                }
            }
        }
    }
    return image;
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setAcceptDrops(true);
    buildUi();
    buildMenusAndToolbar();
    connectUi();
    loadSettings();
    updateGameMetadata();

    resize(1100, 760);
    setMinimumSize(860, 620);
    setWindowTitle(QString("GekkoAOT v%1").arg(QString::fromUtf8(GEKKOAOT_VERSION)));
    setWindowIcon(themedIcon(this, "media-optical", QStyle::SP_DriveDVDIcon));
}

void MainWindow::buildUi()
{
    setStyleSheet(R"QSS(
        QMainWindow, QWidget#Root {
            background: #2b2b2b;
            color: #e8e8e8;
            font-size: 13px;
        }
        QMenuBar, QMenuBar::item {
            background: #2f2f2f;
            color: #e8e8e8;
        }
        QMenuBar::item:selected, QMenu::item:selected { background: #3d3d3d; }
        QMenu {
            background: #323232;
            color: #e8e8e8;
            border: 1px solid #4a4a4a;
        }
        QToolBar {
            background: #313131;
            border-bottom: 1px solid #474747;
            spacing: 4px;
            padding: 4px;
        }
        QToolButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 6px;
            color: #ececec;
        }
        QToolButton:hover { background: #3a3a3a; border-color: #505050; }
        QToolButton:pressed { background: #2d2d2d; }
        QGroupBox {
            background: #313131;
            border: 1px solid #474747;
            border-radius: 6px;
            margin-top: 12px;
            font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 4px;
        }
        QLabel[muted="true"] { color: #b3b3b3; }
        QLabel#GameBanner {
            background: #000000;
            border: 1px solid #2a2a2a;
            border-radius: 3px;
        }
        QLabel#LoaderTitle {
            color: #f0f0f0;
            font-size: 24px;
            font-weight: 700;
        }
        QLabel#LoaderStatus {
            background: #000000;
            color: #e0e0e0;
            font-size: 15px;
            font-weight: 600;
        }
        QLabel#LoaderHint { background: #000000; color: #8f8f8f; }
        QWidget#LoaderOverlay { background: #000000; }
        QPushButton {
            background: #3a3a3a;
            border: 1px solid #565656;
            border-radius: 5px;
            color: #ededed;
            padding: 7px 12px;
            min-height: 26px;
        }
        QPushButton:hover { background: #454545; }
        QPushButton:pressed { background: #303030; }
        QPushButton:disabled {
            color: #909090;
            background: #2f2f2f;
            border-color: #444444;
        }
        QPushButton#DangerButton { color: #ffb6b6; }
        QLineEdit, QComboBox, QSpinBox {
            background: #262626;
            border: 1px solid #565656;
            border-radius: 4px;
            padding: 5px 7px;
            min-height: 22px;
            color: #ececec;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: #d6ae30; }
        QPlainTextEdit {
            background: #181818;
            border: 1px solid #444444;
            border-radius: 4px;
            color: #e2e2e2;
            selection-background-color: #8a6c16;
        }
        QProgressBar#CompileProgress {
            background: #171717;
            border: 1px solid #5b512f;
            border-radius: 4px;
            height: 18px;
            color: #f4dda0;
            font-weight: 700;
            text-align: center;
        }
        QProgressBar#CompileProgress::chunk {
            background: #e0b62f;
            border-radius: 3px;
        }
        QStatusBar {
            background: #2d2d2d;
            color: #d7d7d7;
            border-top: 1px solid #474747;
        }
    )QSS");

    auto* root = new QWidget(this);
    root->setObjectName("Root");
    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_viewFrame = new QWidget(root);
    m_viewFrame->setMinimumHeight(420);
    m_viewStack = new QStackedLayout(m_viewFrame);
    m_viewStack->setContentsMargins(0, 0, 0, 0);
    m_viewStack->setStackingMode(QStackedLayout::StackAll);

    m_gameViewport = new NativeRenderWidget(m_viewFrame);
    m_gameViewport->setObjectName("GameViewport");
    m_gameViewport->setAttribute(Qt::WA_NativeWindow, true);
    // The child XID becomes AuroraGX's presentation target once the native runtime starts.
    // Do not let Qt's backing store paint the same X11 window at the same time:
    // on XWayland that produces black/striped scanline-looking corruption even
    // though the Vulkan swapchain itself is healthy.
    m_gameViewport->setAttribute(Qt::WA_PaintOnScreen, true);
    m_gameViewport->setAttribute(Qt::WA_NoSystemBackground, true);
    m_gameViewport->setAutoFillBackground(false);
    m_gameViewport->setFocusPolicy(Qt::StrongFocus);
    m_gameViewport->setMinimumSize(640, 360);
    m_viewStack->addWidget(m_gameViewport);

    // Native overlay above Aurora's X11/Vulkan child surface. Keep this private
    // to MainWindow.cpp so HUD changes do not churn the public MainWindow ABI.
    g_perfHud = new QLabel(m_gameViewport);
    g_perfHud->setObjectName("PerformanceHud");
    g_perfHud->setAttribute(Qt::WA_NativeWindow, true);
    g_perfHud->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    g_perfHud->setText("VPS --  ·  G-FPS --  ·  FPS --  ·  I-FPS --  ·  Speed --%");
    g_perfHud->setStyleSheet(
        "QLabel#PerformanceHud { color: #ffffff; background: rgba(0,0,0,185); "
        "border: 1px solid rgba(255,255,255,70); border-radius: 5px; padding: 6px 9px; "
        "font-family: monospace; font-weight: 700; }");
    g_perfHud->adjustSize();
    g_perfHud->move(12, 12);
    g_perfHud->hide();

    m_loaderOverlay = new QWidget(m_viewFrame);
    m_loaderOverlay->setObjectName("LoaderOverlay");
    // Do not mark the Qt loader as an opaque paint surface. Its child labels
    // are transparent by default, and rapid status updates can otherwise leave
    // old glyphs in the backing store (the "stacked text" artefact).
    m_loaderOverlay->setAttribute(Qt::WA_StyledBackground, true);
    auto* loaderOuter = new QVBoxLayout(m_loaderOverlay);
    loaderOuter->setContentsMargins(70, 70, 70, 70);
    loaderOuter->addStretch(2);

    m_bannerLabel = new QLabel(m_loaderOverlay);
    m_bannerLabel->setObjectName("GameBanner");
    m_bannerLabel->setAlignment(Qt::AlignCenter);
    m_bannerLabel->setFixedSize(384, 128);
    m_bannerLabel->setVisible(false);
    loaderOuter->addWidget(m_bannerLabel, 0, Qt::AlignHCenter);
    loaderOuter->addSpacing(18);

    m_loaderTitle = new QLabel("GekkoAOT", m_loaderOverlay);
    m_loaderTitle->setObjectName("LoaderTitle");
    m_loaderTitle->setAlignment(Qt::AlignCenter);
    m_loaderTitle->setWordWrap(true);
    loaderOuter->addWidget(m_loaderTitle);

    loaderOuter->addSpacing(16);
    m_loaderStatus = new QLabel("Open a GameCube/Wii disc image to start", m_loaderOverlay);
    m_loaderStatus->setObjectName("LoaderStatus");
    m_loaderStatus->setAlignment(Qt::AlignCenter);
    m_loaderStatus->setMinimumHeight(28);
    m_loaderStatus->setWordWrap(false);
    loaderOuter->addWidget(m_loaderStatus);

    loaderOuter->addSpacing(18);
    m_progress = new QProgressBar(m_loaderOverlay);
    m_progress->setObjectName("CompileProgress");
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_progress->setFormat("%p%");
    m_progress->setMaximumWidth(620);
    loaderOuter->addWidget(m_progress, 0, Qt::AlignHCenter);

    loaderOuter->addSpacing(10);
    m_loaderHint = new QLabel("File → Open Game", m_loaderOverlay);
    m_loaderHint->setObjectName("LoaderHint");
    m_loaderHint->setAlignment(Qt::AlignCenter);
    loaderOuter->addWidget(m_loaderHint);
    loaderOuter->addStretch(3);

    m_viewStack->addWidget(m_loaderOverlay);
    m_viewStack->setCurrentWidget(m_loaderOverlay);
    outer->addWidget(m_viewFrame, 1);

    // Advanced settings live in their own non-modal window. Keeping this out
    // of the main vertical layout prevents the loader, settings and log panes
    // from crushing each other when the window is not tall enough.
    m_advancedDialog = new QDialog(this);
    m_advancedDialog->setWindowTitle("Advanced Settings");
    m_advancedDialog->setModal(false);
    m_advancedDialog->setMinimumSize(760, 430);
    m_advancedDialog->resize(900, 620);
    auto* advancedDialogLayout = new QVBoxLayout(m_advancedDialog);
    advancedDialogLayout->setContentsMargins(12, 12, 12, 12);
    advancedDialogLayout->setSpacing(10);

    m_advancedPanel = new QWidget(m_advancedDialog);
    auto* advancedOuter = new QVBoxLayout(m_advancedPanel);
    advancedOuter->setContentsMargins(0, 0, 0, 0);
    advancedOuter->setSpacing(10);

    auto* settingsForm = new QFormLayout();
    settingsForm->setHorizontalSpacing(18);
    settingsForm->setVerticalSpacing(10);
    settingsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_backend = new QComboBox(m_advancedPanel);
    m_backend->addItem("LLVM AOT", "llvm");
    m_backend->addItem("Portable C AOT", "c");

    m_profile = new QComboBox(m_advancedPanel);
    m_profile->addItem("Native / Max performance", "native");
    m_profile->addItem("Modern x86-64-v3", "modern");
    m_profile->addItem("Compat x86-64-v2", "compat");
    m_profile->addItem("Baseline portable", "baseline");
    m_profile->addItem("Lockstep / verification", "lockstep");

    m_toolchain = new QComboBox(m_advancedPanel);
    m_toolchain->addItem("Clang", "clang");
    m_toolchain->addItem("Auto", "auto");
    m_toolchain->addItem("GCC", "gcc");
    m_toolchain->addItem("MSVC", "msvc");

    m_nativeAbi = new QComboBox(m_advancedPanel);
    m_nativeAbi->addItem("Unrestricted (fastest)", "unrestricted");
    m_nativeAbi->addItem("Compact", "compact");
    m_nativeAbi->addItem("Off / compatibility", "off");

    auto* llvmRow = new QWidget(m_advancedPanel);
    auto* llvmLayout = new QHBoxLayout(llvmRow);
    llvmLayout->setContentsMargins(0, 0, 0, 0);
    llvmLayout->setSpacing(8);
    m_llvmDir = new QLineEdit(llvmRow);
    m_llvmDir->setPlaceholderText("Auto-detect LLVM 20 / 19");
    m_llvmBrowse = new QPushButton("Browse", llvmRow);
    llvmLayout->addWidget(m_llvmDir, 1);
    llvmLayout->addWidget(m_llvmBrowse);

    m_jobs = new QSpinBox(m_advancedPanel);
    m_jobs->setRange(1, 256);
    m_jobs->setValue(qMax(1, QThread::idealThreadCount()));

    m_runArgs = new QLineEdit(m_advancedPanel);
    m_runArgs->setPlaceholderText("Additional native-run arguments");

    settingsForm->addRow("Backend", m_backend);
    settingsForm->addRow("Performance", m_profile);
    settingsForm->addRow("Toolchain", m_toolchain);
    settingsForm->addRow("Native ABI", m_nativeAbi);
    settingsForm->addRow("LLVM", llvmRow);
    settingsForm->addRow("Parallel jobs", m_jobs);
    settingsForm->addRow("Extra runner args", m_runArgs);
    advancedOuter->addLayout(settingsForm);

    auto* asyncHint = mutedLabel(
        "The build runs asynchronously. You can inspect logs and change settings while it runs; "
        "changes apply to the next launch.", m_advancedPanel);
    asyncHint->setWordWrap(true);
    advancedOuter->addWidget(asyncHint);

    auto* advancedButtons = new QHBoxLayout();
    advancedButtons->setSpacing(8);
    m_pgo = new QPushButton("Train PGO", m_advancedPanel);
    m_pgo->setToolTip("Run representative gameplay, then press Stop to merge the profile and build the optimized LLVM AOT module.");
    m_pgo->setIcon(themedIcon(this, "system-run", QStyle::SP_BrowserReload));
    m_inspect = new QPushButton("Analyze", m_advancedPanel);
    m_inspect->setIcon(themedIcon(this, "edit-find", QStyle::SP_FileDialogContentsView));
    m_compile = new QPushButton("Compile only", m_advancedPanel);
    m_compile->setIcon(themedIcon(this, "system-run", QStyle::SP_ArrowForward));
    m_buildTools = new QPushButton("Rebuild engine", m_advancedPanel);
    m_buildTools->setIcon(themedIcon(this, "view-refresh", QStyle::SP_BrowserReload));
    m_clean = new QPushButton("Clean cache", m_advancedPanel);
    m_clean->setObjectName("DangerButton");
    m_clean->setIcon(themedIcon(this, "edit-delete", QStyle::SP_TrashIcon));
    for (auto* button : {m_pgo, m_inspect, m_compile, m_buildTools, m_clean})
        advancedButtons->addWidget(button);
    advancedButtons->addStretch(1);
    advancedOuter->addLayout(advancedButtons);

    auto* debugRow = new QHBoxLayout();
    m_showLogCheck = new QCheckBox("Show Log", m_advancedPanel);
    m_showLogCheck->setToolTip("Logs are always collected; this only shows or hides them.");
    debugRow->addWidget(m_showLogCheck);
    debugRow->addStretch(1);
    advancedOuter->addLayout(debugRow);

    auto* revisions = mutedLabel(
        QString("Pinned: DolRecomp %1 · Aurora %2 · nod %3")
            .arg(kDolRecomp, kAurora, kNod),
        m_advancedPanel);
    revisions->setTextInteractionFlags(Qt::TextSelectableByMouse);
    advancedOuter->addWidget(revisions);
    advancedDialogLayout->addWidget(m_advancedPanel);

    m_logPanel = new QGroupBox("Log", m_advancedDialog);
    auto* logLayout = new QVBoxLayout(m_logPanel);
    logLayout->setContentsMargins(10, 14, 10, 10);
    m_log = new QPlainTextEdit(m_logPanel);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(30000);
    QFont mono("monospace");
    mono.setStyleHint(QFont::Monospace);
    m_log->setFont(mono);
    m_log->setMinimumHeight(220);
    logLayout->addWidget(m_log);
    m_logPanel->setVisible(false);
    advancedDialogLayout->addWidget(m_logPanel, 1);

    setCentralWidget(root);

    auto* revStatus = new QLabel(
        QString("DR %1 · Aurora %2 · nod %3").arg(kDolRecomp, kAurora, kNod), this);
    revStatus->setProperty("muted", true);
    statusBar()->addPermanentWidget(revStatus);
    statusBar()->showMessage("Ready");
}

void MainWindow::buildMenusAndToolbar()
{
    auto* fileMenu = menuBar()->addMenu("&File");
    m_openAction = fileMenu->addAction(themedIcon(this, "document-open", QStyle::SP_DialogOpenButton),
                                       "Open Game…");
    fileMenu->addSeparator();
    fileMenu->addAction("Quit", this, &QWidget::close);

    auto* emuMenu = menuBar()->addMenu("&Emulation");
    m_playAction = emuMenu->addAction(themedIcon(this, "media-playback-start", QStyle::SP_MediaPlay),
                                      "Play");
    m_stopAction = emuMenu->addAction(themedIcon(this, "media-playback-stop", QStyle::SP_MediaStop),
                                      "Stop");
    emuMenu->addSeparator();
    m_fullscreenAction = emuMenu->addAction(
        themedIcon(this, "view-fullscreen", QStyle::SP_TitleBarMaxButton),
        "Fullscreen Game View");
    m_fullscreenAction->setCheckable(true);

    // Keep Alt+Enter independent from the menu action. QAction shortcuts can
    // stop toggling after a fullscreen state transition on some Qt/XWayland
    // combinations because the checked action and the native window state get
    // out of sync. Application-scoped QShortcuts always consult the actual
    // QWindow state through toggleGameViewFullscreen().
    auto* fullscreenReturn = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Return), this);
    fullscreenReturn->setContext(Qt::ApplicationShortcut);
    connect(fullscreenReturn, &QShortcut::activated, this, &MainWindow::toggleGameViewFullscreen);
    auto* fullscreenEnter = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Enter), this);
    fullscreenEnter->setContext(Qt::ApplicationShortcut);
    connect(fullscreenEnter, &QShortcut::activated, this, &MainWindow::toggleGameViewFullscreen);

    m_gameEnhancementsMenu = menuBar()->addMenu("Game &Enhancements");
    m_gameEnhancementsMenu->setToolTipsVisible(true);
    m_gameEnhancementsMenu->menuAction()->setEnabled(false);

    auto* graphicsMenu = menuBar()->addMenu("&Graphics");
    m_graphicsAction = graphicsMenu->addAction(
        themedIcon(this, "preferences-desktop-display", QStyle::SP_ComputerIcon),
        "Graphics Settings…");

    auto* inputMenu = menuBar()->addMenu("&Input");
    m_inputAction = inputMenu->addAction(themedIcon(this, "input-gaming", QStyle::SP_DriveNetIcon),
                                         "Input Settings…");

    auto* toolsMenu = menuBar()->addMenu("&Tools");
    m_analyzeAction = toolsMenu->addAction(themedIcon(this, "edit-find", QStyle::SP_FileDialogContentsView),
                                           "Analyze");
    m_compileAction = toolsMenu->addAction(themedIcon(this, "system-run", QStyle::SP_ArrowForward),
                                           "Compile only");
    m_pgoAction = toolsMenu->addAction(themedIcon(this, "system-run", QStyle::SP_BrowserReload),
                                       "Train PGO");
    toolsMenu->addSeparator();
    m_buildToolsAction = toolsMenu->addAction(
        themedIcon(this, "view-refresh", QStyle::SP_BrowserReload), "Rebuild Engine");
    m_cleanAction = toolsMenu->addAction(themedIcon(this, "edit-delete", QStyle::SP_TrashIcon),
                                         "Clean Cache");
    toolsMenu->addSeparator();
    m_advancedAction = toolsMenu->addAction(
        themedIcon(this, "preferences-system", QStyle::SP_FileDialogDetailedView), "Advanced");
    m_advancedAction->setCheckable(true);

    auto* helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("About GekkoAOT", [this] {
        statusBar()->showMessage(
            QString("GekkoAOT v%1 — native GameCube AOT runtime with DolRecomp + AuroraGX + nod")
                .arg(QStringLiteral(GEKKOAOT_VERSION)), 4000);
    });

    m_mainToolbar = addToolBar("Main");
    m_mainToolbar->setMovable(false);
    m_mainToolbar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_mainToolbar->setIconSize(QSize(22, 22));
    m_mainToolbar->addAction(m_openAction);
    m_mainToolbar->addSeparator();
    m_mainToolbar->addAction(m_playAction);
    m_mainToolbar->addAction(m_stopAction);
    m_mainToolbar->addSeparator();
    m_mainToolbar->addAction(m_graphicsAction);
    m_mainToolbar->addAction(m_inputAction);
    m_mainToolbar->addSeparator();
    m_gameEnhancementsButton = new QToolButton(m_mainToolbar);
    m_gameEnhancementsButton->setText("Enhancements");
    m_gameEnhancementsButton->setIcon(
        themedIcon(this, "applications-games", QStyle::SP_DesktopIcon));
    m_gameEnhancementsButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_gameEnhancementsButton->setPopupMode(QToolButton::InstantPopup);
    m_gameEnhancementsButton->setMenu(m_gameEnhancementsMenu);
    m_gameEnhancementsButton->setEnabled(false);
    m_mainToolbar->addWidget(m_gameEnhancementsButton);
    m_mainToolbar->addSeparator();
    m_mainToolbar->addAction(m_advancedAction);
}

void MainWindow::connectUi()
{
    connect(m_llvmBrowse, &QPushButton::clicked, this, &MainWindow::browseLlvmDir);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::browseIso);
    connect(m_playAction, &QAction::triggered, this, [this] { launchAction("run"); });
    connect(m_stopAction, &QAction::triggered, this, [this] {
        if (m_activeAction == "pgo" && m_progressStage == "runtime")
        {
            // PGO needs a normal runtime shutdown so compiler-rt flushes the
            // .profraw counters. Ask the native runtime to stop cleanly instead of
            // killing the process group used by the generic Stop path.
            QSaveFile stopFile(QDir(frontendUserDir()).filePath("pgo-stop.request"));
            if (!stopFile.open(QIODevice::WriteOnly) || stopFile.write("stop\n") != 5 ||
                !stopFile.commit())
            {
                statusBar()->showMessage("Could not finish PGO training", 5000);
                return;
            }
            m_progressStage = "pgo-finalize";
            showLoader("PGO", "Finishing training and optimizing the module…", 99);
            return;
        }
        m_stopRequested = true;
        m_controller.stop();
    });
    connect(m_graphicsAction, &QAction::triggered, this, &MainWindow::showGraphicsConfig);
    connect(m_inputAction, &QAction::triggered, this, &MainWindow::showInputConfig);
    connect(m_advancedAction, &QAction::toggled, this, &MainWindow::setAdvancedVisible);
    connect(m_fullscreenAction, &QAction::triggered, this, [this] { toggleGameViewFullscreen(); });
    connect(m_showLogCheck, &QCheckBox::toggled, this, &MainWindow::setLogVisible);
    connect(m_advancedDialog, &QDialog::finished, this, [this] {
        if (m_advancedAction && m_advancedAction->isChecked())
            m_advancedAction->setChecked(false);
    });

    connect(m_backend, &QComboBox::currentIndexChanged, this, [this] {
        const bool llvm = m_backend->currentData().toString() == "llvm";
        m_nativeAbi->setEnabled(llvm);
        const bool hasGame = QFileInfo::exists(m_isoPath) && looksLikeDiscImage(m_isoPath);
        const bool canPgo = llvm && hasGame && !m_controller.isRunning();
        if (m_pgo)
            m_pgo->setEnabled(canPgo);
        if (m_pgoAction)
            m_pgoAction->setEnabled(canPgo);
    });

    connect(m_pgo, &QPushButton::clicked, this, [this] { launchAction("pgo"); });
    connect(m_inspect, &QPushButton::clicked, this, [this] { launchAction("inspect"); });
    connect(m_compile, &QPushButton::clicked, this, [this] { launchAction("compile"); });
    connect(m_buildTools, &QPushButton::clicked, this, [this] { launchAction("tools"); });
    connect(m_clean, &QPushButton::clicked, this, [this] { launchAction("clean"); });

    connect(m_analyzeAction, &QAction::triggered, this, [this] { launchAction("inspect"); });
    connect(m_compileAction, &QAction::triggered, this, [this] { launchAction("compile"); });
    connect(m_pgoAction, &QAction::triggered, this, [this] { launchAction("pgo"); });
    connect(m_buildToolsAction, &QAction::triggered, this, [this] { launchAction("tools"); });
    connect(m_cleanAction, &QAction::triggered, this, [this] { launchAction("clean"); });

    connect(&m_controller, &BuildController::outputReady, m_log,
            [this](const QString& text) {
                m_log->moveCursor(QTextCursor::End);
                m_log->insertPlainText(text);
                m_log->moveCursor(QTextCursor::End);
                updateStatusFromOutput(text);
            });
    connect(&m_controller, &BuildController::runningChanged, this, &MainWindow::setBusy);
    connect(&m_controller, &BuildController::finished, this,
            [this](int code, QProcess::ExitStatus status) {
                const bool ok = code == 0 && status == QProcess::NormalExit;
                if (m_stopRequested)
                {
                    showIdleViewport("Stopped — press Play to launch again");
                    statusBar()->showMessage("Stopped", 2000);
                }
                else if (ok)
                {
                    if (m_activeAction == "run" && m_runtimeStarted)
                        showIdleViewport("Game closed — press Play to launch again");
                    else if (m_activeAction == "pgo")
                        showIdleViewport("PGO session finished");
                    else if (m_activeAction == "clean")
                        showIdleViewport("Cache cleaned");
                    else if (m_activeAction != "run")
                        showIdleViewport("Ready");
                    statusBar()->showMessage("Ready", 2000);
                }
                else
                {
                    showLoader(gameTitleForPath(m_isoPath),
                               "Build or launch failed — see Advanced → Show Log", 100);
                    setAdvancedVisible(true);
                    setLogVisible(true);
                    statusBar()->showMessage("Last command failed", 4000);
                }
                const char* finishStatus = m_stopRequested
                    ? "stopped"
                    : (status == QProcess::NormalExit ? "normal" : "crashed");
                m_log->appendPlainText(
                    QString("\n[gekkoaot] finished: code=%1 status=%2\n")
                        .arg(code)
                        .arg(finishStatus));
                m_activeAction.clear();
                m_runtimeStarted = false;
                m_stopRequested = false;
            });
}

void MainWindow::loadSettings()
{
    QSettings s("GekkoAOT", "GekkoAOT");
    m_isoPath = s.value("iso").toString();
    m_llvmDir->setText(s.value("llvmDir").toString());
    m_runArgs->setText(s.value("runArgs").toString());
    m_jobs->setValue(s.value("jobs", qMax(1, QThread::idealThreadCount())).toInt());

    m_graphicsBackend = s.value("graphics/backend", "Vulkan").toString();
    m_resolution = s.value("graphics/resolution", "1920x1080").toString();
    m_aspectMode = s.value("graphics/aspectMode", "4:3").toString();
    if (m_aspectMode != "auto" && m_aspectMode != "4:3" &&
        m_aspectMode != "16:9" && m_aspectMode != "stretch")
        m_aspectMode = "4:3";
    // "auto" means native X11 on an X11 session and an XWayland child on a
    // Wayland session. Pure-Wayland sessions without DISPLAY fall back to a
    // separate native Wayland runtime window because Wayland cannot reparent
    // a surface created by another process into a Qt widget.
    m_windowSystem = "auto";
    m_fullscreen = s.value("graphics/fullscreen", false).toBool();
    m_showFps = s.value("graphics/showFps", true).toBool();
    m_fpsLimit = qBound(30, s.value("graphics/fpsLimit", 120).toInt(), 360);
    m_controllers = s.value("input/controllers").toStringList();
    while (m_controllers.size() < 4)
        m_controllers.append(QString{});
    if (m_controllers.size() > 4)
        m_controllers = m_controllers.mid(0, 4);

    const auto restoreCombo = [&s](QComboBox* combo, const char* key, const QString& fallback) {
        const QString value = s.value(key, fallback).toString();
        const int index = combo->findData(value);
        if (index >= 0)
            combo->setCurrentIndex(index);
    };
    restoreCombo(m_backend, "backend", "llvm");
    restoreCombo(m_profile, "profile", "native");
    restoreCombo(m_toolchain, "toolchain", "clang");
    restoreCombo(m_nativeAbi, "nativeAbi", "unrestricted");
    m_nativeAbi->setEnabled(m_backend->currentData().toString() == "llvm");
}

void MainWindow::saveSettings() const
{
    QSettings s("GekkoAOT", "GekkoAOT");
    s.setValue("iso", m_isoPath);
    s.setValue("llvmDir", m_llvmDir->text());
    s.setValue("runArgs", m_runArgs->text());
    s.setValue("jobs", m_jobs->value());
    s.setValue("backend", m_backend->currentData());
    s.setValue("profile", m_profile->currentData());
    s.setValue("toolchain", m_toolchain->currentData());
    s.setValue("nativeAbi", m_nativeAbi->currentData());

    s.setValue("graphics/backend", m_graphicsBackend);
    s.setValue("graphics/resolution", m_resolution);
    s.setValue("graphics/aspectMode", m_aspectMode);
    s.setValue("graphics/fullscreen", m_fullscreen);
    s.setValue("graphics/showFps", m_showFps);
    s.setValue("graphics/fpsLimit", m_fpsLimit);
    s.setValue("input/controllers", m_controllers);
}

void MainWindow::browseIso()
{
    const QString startDir = m_isoPath.isEmpty() ? QString{} : QFileInfo(m_isoPath).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        this, "Choose a GameCube/Wii game", startDir,
        "Game sources (*.iso *.gcm *.wbfs *.rvz *.wia *.gcz *.dol);;Disc images (*.iso *.gcm *.wbfs *.rvz *.wia *.gcz);;GameCube DOL (*.dol);;All files (*)",
        nullptr, kPortableFileDialogOptions);
    if (!path.isEmpty())
        setIsoPath(path, true);
}

void MainWindow::browseLlvmDir()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, "Select LLVM CMake directory", {},
        QFileDialog::ShowDirsOnly | kPortableFileDialogOptions);
    if (!path.isEmpty())
        m_llvmDir->setText(path);
}

void MainWindow::setIsoPath(const QString& path, bool autoPlay)
{
    m_isoPath = QFileInfo(path).absoluteFilePath();
    updateGameMetadata();
    updateLoaderBanner();
    saveSettings();

    QString error;
    if (!writeFrontendConfig(&error) && !error.isEmpty())
    {
        m_log->appendPlainText("[gekkoaot] settings: " + error + "\n");
        setAdvancedVisible(true);
        setLogVisible(true);
        return;
    }

    if (autoPlay && QFileInfo::exists(m_isoPath) && looksLikeDiscImage(m_isoPath))
    {
        // Let the file dialog fully close and repaint the black loader before
        // starting the compiler process.
        QTimer::singleShot(0, this, [this] { launchAction("run"); });
    }
}

QString MainWindow::slugForPath(const QString& path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    QString slug = info.completeBaseName();
    if (info.suffix().compare(QStringLiteral("dol"), Qt::CaseInsensitive) == 0 &&
        slug.compare(QStringLiteral("main"), Qt::CaseInsensitive) == 0)
    {
        QDir parent = info.dir();
        QString label = parent.dirName();
        if (label.compare(QStringLiteral("sys"), Qt::CaseInsensitive) == 0 && parent.cdUp())
            label = parent.dirName();
        if (!label.isEmpty())
            slug = label;
    }
    slug.replace(QRegularExpression("[^A-Za-z0-9]+"), "-");
    slug.remove(QRegularExpression("^-+|-+$"));
    return slug;
}

QString MainWindow::gameTitleForPath(const QString& path)
{
    if (path.isEmpty())
        return {};

    const QFileInfo sourceInfo(path);
    const QString fallback = sourceInfo.completeBaseName();

    const auto titleFromBoot = [](const QString& bootPath) -> QString {
        QFile boot(bootPath);
        if (!boot.open(QIODevice::ReadOnly) || !boot.seek(0x20))
            return {};
        QByteArray raw = boot.read(0x3e0);
        const qsizetype nul = raw.indexOf('\0');
        if (nul >= 0)
            raw.truncate(nul);
        QString title = QString::fromLatin1(raw).trimmed();
        title.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")));
        return title.trimmed();
    };

    // Prefer the exact boot.bin exported by the disc reader.  Unlike the
    // source filename this is metadata owned by the game itself and also works
    // for renamed images once their executable cache has been prepared.
    const QDir stateRoot(runtimeStateRoot());
    const QString cachedBoot =
        stateRoot.filePath("cache/discs/" + slugForPath(path) + "/sys/boot.bin");
    if (const QString title = titleFromBoot(cachedBoot); !title.isEmpty())
        return title;

    // DOL package input: boot.bin normally lives beside sys/main.dol.
    if (sourceInfo.suffix().compare(QStringLiteral("dol"), Qt::CaseInsensitive) == 0)
    {
        QDir root = sourceInfo.dir();
        if (root.dirName().compare(QStringLiteral("sys"), Qt::CaseInsensitive) == 0)
            root.cdUp();
        const QStringList bootCandidates = {
            root.filePath("sys/boot.bin"), sourceInfo.dir().filePath("boot.bin"), root.filePath("boot.bin")};
        for (const QString& candidate : bootCandidates)
        {
            if (const QString title = titleFromBoot(candidate); !title.isEmpty())
                return title;
        }
    }
    else
    {
        // ISO/GCM contain boot.bin verbatim at offset zero.  Compressed formats
        // (RVZ/WIA/GCZ/WBFS) intentionally wait for the cached exported header.
        const QString suffix = sourceInfo.suffix().toLower();
        if (suffix == QStringLiteral("iso") || suffix == QStringLiteral("gcm"))
        {
            if (const QString title = titleFromBoot(path); !title.isEmpty())
                return title;
        }
    }

    // Compatibility fallback for the very first frame before a compressed
    // image has produced its executable cache.
    return fallback;
}

QString MainWindow::discIdForPath(const QString& path)
{
    const QString cachedIdPath =
        QDir(runtimeStateRoot()).filePath("cache/discs/" + slugForPath(path) + "/meta/disc-id.txt");
    QFile cached(cachedIdPath);
    if (cached.open(QIODevice::ReadOnly))
    {
        const QString cachedId = QString::fromLatin1(cached.readAll()).trimmed();
        if (QRegularExpression("^[A-Za-z0-9]{6}$").match(cachedId).hasMatch())
            return cachedId;
    }

    const QFileInfo sourceInfo(path);
    if (sourceInfo.suffix().compare(QStringLiteral("dol"), Qt::CaseInsensitive) == 0)
    {
        QDir root = sourceInfo.dir();
        if (root.dirName().compare(QStringLiteral("sys"), Qt::CaseInsensitive) == 0)
            root.cdUp();
        const QStringList bootCandidates = {
            root.filePath("sys/boot.bin"), sourceInfo.dir().filePath("boot.bin"), root.filePath("boot.bin")};
        for (const QString& candidate : bootCandidates)
        {
            QFile boot(candidate);
            if (!boot.open(QIODevice::ReadOnly))
                continue;
            const QByteArray raw = boot.read(6);
            const QString id = QString::fromLatin1(raw);
            if (raw.size() == 6 && QRegularExpression("^[A-Za-z0-9]{6}$").match(id).hasMatch())
                return id;
        }
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray id = file.read(6);
    if (id.size() != 6)
        return {};
    const QString text = QString::fromLatin1(id);
    if (!QRegularExpression("^[A-Za-z0-9]{6}$").match(text).hasMatch())
        return {};
    return text;
}

bool MainWindow::looksLikeDiscImage(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "iso" || suffix == "gcm" || suffix == "wbfs" || suffix == "rvz" ||
           suffix == "wia" || suffix == "gcz" || suffix == "dol";
}


QImage MainWindow::gameCubeBannerForPath(const QString& path)
{
    // Compressed containers cannot be parsed as a raw disc with QFile. Once
    // DiscIO has filled the hidden executable cache, use its opening.bnr.
    const QString cachedBanner =
        QDir(runtimeStateRoot()).filePath("cache/discs/" + slugForPath(path) + "/meta/opening.bnr");
    QFile cached(cachedBanner);
    if (cached.open(QIODevice::ReadOnly))
    {
        const QImage image = decodeGameCubeBanner(cached.readAll());
        if (!image.isNull())
            return image;
    }

    const QFileInfo sourceInfo(path);
    if (sourceInfo.suffix().compare(QStringLiteral("dol"), Qt::CaseInsensitive) == 0)
    {
        QDir root = sourceInfo.dir();
        if (root.dirName().compare(QStringLiteral("sys"), Qt::CaseInsensitive) == 0)
            root.cdUp();
        const QStringList bannerCandidates = {
            root.filePath("files/opening.bnr"), sourceInfo.dir().filePath("opening.bnr"),
            root.filePath("opening.bnr"), root.filePath("assets/opening.bnr")};
        for (const QString& candidate : bannerCandidates)
        {
            QFile banner(candidate);
            if (!banner.open(QIODevice::ReadOnly))
                continue;
            const QImage image = decodeGameCubeBanner(banner.readAll());
            if (!image.isNull())
                return image;
        }
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0x440)
        return {};

    if (!file.seek(0))
        return {};
    const QByteArray header = file.read(0x440);
    if (header.size() != 0x440 || be32(header, 0x1C) != 0xC2339F3Du)
        return {};

    const quint32 fstOffset = be32(header, 0x424);
    const quint32 fstSize = be32(header, 0x428);
    if (fstOffset == 0 || fstSize < 12 || fstSize > 64u * 1024u * 1024u ||
        static_cast<quint64>(fstOffset) + fstSize > static_cast<quint64>(file.size()))
        return {};

    if (!file.seek(fstOffset))
        return {};
    const QByteArray fst = file.read(fstSize);
    if (fst.size() != static_cast<qsizetype>(fstSize))
        return {};

    const quint32 count = be32(fst, 8);
    if (count < 1 || count > 1000000u || static_cast<quint64>(count) * 12u > fstSize)
        return {};
    const qsizetype strings = static_cast<qsizetype>(count) * 12;

    quint32 bannerOffset = 0;
    quint32 bannerSize = 0;
    for (quint32 i = 1; i < count; ++i)
    {
        const qsizetype base = static_cast<qsizetype>(i) * 12;
        const quint32 typeName = be32(fst, base);
        if ((typeName >> 24) != 0)
            continue;
        const quint32 nameOffset = typeName & 0x00FFFFFFu;
        const qsizetype nameStart = strings + static_cast<qsizetype>(nameOffset);
        if (nameStart < strings || nameStart >= fst.size())
            continue;
        const qsizetype nameEnd = fst.indexOf('\0', nameStart);
        if (nameEnd < 0)
            continue;
        const QString name = QString::fromLatin1(fst.constData() + nameStart, nameEnd - nameStart);
        if (name.compare(QStringLiteral("opening.bnr"), Qt::CaseInsensitive) == 0)
        {
            bannerOffset = be32(fst, base + 4);
            bannerSize = be32(fst, base + 8);
            break;
        }
    }

    constexpr quint32 textureOffset = 0x20;
    constexpr quint32 textureBytes = 96u * 32u * 2u;
    if (!bannerOffset || bannerSize < textureOffset + textureBytes ||
        static_cast<quint64>(bannerOffset) + bannerSize > static_cast<quint64>(file.size()))
        return {};

    if (!file.seek(bannerOffset))
        return {};
    return decodeGameCubeBanner(file.read(textureOffset + textureBytes));
}

void MainWindow::updateLoaderBanner()
{
    if (!m_bannerLabel)
        return;
    const QImage banner = gameCubeBannerForPath(m_isoPath);
    if (banner.isNull())
    {
        m_bannerLabel->clear();
        m_bannerLabel->hide();
        return;
    }
    m_bannerLabel->setPixmap(QPixmap::fromImage(banner).scaled(
        m_bannerLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    m_bannerLabel->show();
}

QString MainWindow::frontendUserDir() const
{
    QString slug = slugForPath(m_isoPath);
    if (slug.isEmpty())
        slug = "default";
    return QDir(runtimeStateRoot()).filePath("user/" + slug);
}

bool MainWindow::writeFrontendConfig(QString* error) const
{
    if (m_isoPath.isEmpty())
        return true;

    const QString userDir = frontendUserDir();
    if (!QDir().mkpath(userDir))
    {
        if (error)
            *error = "cannot create GekkoAOT user directory: " + userDir;
        return false;
    }

    QSaveFile file(QDir(userDir).filePath("config.ini"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        if (error)
            *error = "cannot write " + file.fileName();
        return false;
    }

    QTextStream out(&file);
    out << "# Generated by GekkoAOT\n"
           "# GekkoAOT native frontend settings\n"
           "[Video]\n"
        << "resolution=" << m_resolution << "\n"
        << "backend=" << m_graphicsBackend << "\n"
        << "aspect_ratio=" << m_aspectMode << "\n"
        << "fullscreen=" << (m_fullscreen ? "true" : "false") << "\n"
        << "show_fps_in_title=" << (m_showFps ? "true" : "false") << "\n"
        << "fps_limit=" << m_fpsLimit << "\n"
           "[Input]\n";

    const int controllerCount = qMin(4, static_cast<int>(m_controllers.size()));
    for (int i = 0; i < controllerCount; ++i)
    {
        QString device = m_controllers.at(i).trimmed();
        device.remove('\r');
        device.remove('\n');
        if (!device.isEmpty())
            out << "controller" << (i + 1) << '=' << device << "\n";
    }

    out << "[Netplay]\n"
           "nickname=Player\n"
           "address=127.0.0.1\n"
           "port=2626\n"
           "buffer=auto\n";

    out << "[GameEnhancements]\n"
        << "disc_id=" << discIdForPath(m_isoPath) << "\n";
    const auto enhancements = gameEnhancementEnvironment();
    for (auto it = enhancements.cbegin(); it != enhancements.cend(); ++it)
    {
        QString value = it.value();
        value.remove('\r');
        value.remove('\n');
        out << it.key() << '=' << value << "\n";
    }

    if (!file.commit())
    {
        if (error)
            *error = "cannot commit " + file.fileName();
        return false;
    }
    return true;
}

void MainWindow::updateGameMetadata()
{
    const QFileInfo info(m_isoPath);
    const bool valid = info.isFile() && looksLikeDiscImage(m_isoPath);
    const QString id = valid ? discIdForPath(m_isoPath) : QString{};
    updateGameEnhancementsMenu();

    if (valid)
    {
        const QString idText = id.isEmpty() ? QStringLiteral("disc image") : id;
        statusBar()->showMessage(QString("%1  [%2]").arg(info.fileName(), idText));
        if (!m_controller.isRunning())
            showIdleViewport("Ready — Play starts automatically when a new game source is opened");
    }
    else if (!m_controller.isRunning())
    {
        showIdleViewport();
    }

    if (!m_controller.isRunning())
    {
        if (m_playAction)
            m_playAction->setEnabled(valid);
        if (m_stopAction)
            m_stopAction->setEnabled(false);
        if (m_analyzeAction)
            m_analyzeAction->setEnabled(valid);
        if (m_compileAction)
            m_compileAction->setEnabled(valid);
        const bool canPgo = valid && m_backend->currentData().toString() == "llvm";
        if (m_pgoAction)
            m_pgoAction->setEnabled(canPgo);
        if (m_pgo)
            m_pgo->setEnabled(canPgo);
        if (m_inspect)
            m_inspect->setEnabled(valid);
        if (m_compile)
            m_compile->setEnabled(valid);
    }
}

QMap<QString, QString> MainWindow::gameEnhancementEnvironment() const
{
    QMap<QString, QString> environment;
    const QString discId = discIdForPath(m_isoPath);
    const QJsonObject pack = loadGamePackJson(discId);
    const QJsonObject ui = pack.value("ui").toObject();
    const QJsonArray options = ui.value("options").toArray();

    for (const QJsonValue& item : options)
    {
        const QJsonObject option = item.toObject();
        const QString env = option.value("env").toString();
        if (env.isEmpty())
            continue;
        const QString value = storedGameOptionValue(discId, option);
        if (!value.isEmpty())
            environment.insert(env, value);
    }
    return environment;
}

void MainWindow::gameEnhancementChanged(const QString& label)
{
    saveSettings();
    QString error;
    if (!writeFrontendConfig(&error))
    {
        statusBar()->showMessage(error, 6000);
        return;
    }
    if (m_controller.isRunning())
        statusBar()->showMessage(label + " saved — live camera and timing update requested", 4500);
    else
        statusBar()->showMessage(label + " saved — applies on next Play", 3000);
}

void MainWindow::updateGameEnhancementsMenu()
{
    if (!m_gameEnhancementsMenu)
        return;

    m_gameEnhancementsMenu->clear();
    const QString discId = discIdForPath(m_isoPath);
    const QJsonObject pack = loadGamePackJson(discId);
    const QJsonObject ui = pack.value("ui").toObject();
    const QJsonArray options = ui.value("options").toArray();
    const bool supported = !discId.isEmpty() && !pack.isEmpty() && !options.isEmpty();
    m_gameEnhancementsMenu->menuAction()->setEnabled(supported);
    if (m_gameEnhancementsButton)
        m_gameEnhancementsButton->setEnabled(supported);

    if (!supported)
        return;

    const QString title = ui.value("title").toString(pack.value("name").toString(discId));
    auto* titleAction = m_gameEnhancementsMenu->addAction(title + "  [" + discId + "]");
    titleAction->setEnabled(false);
    m_gameEnhancementsMenu->addSeparator();

    for (const QJsonValue& item : options)
    {
        const QJsonObject option = item.toObject();
        const QString type = option.value("type").toString();
        const QString label = option.value("label").toString(option.value("id").toString());
        const QString value = storedGameOptionValue(discId, option);

        if (type == "toggle")
        {
            auto* action = m_gameEnhancementsMenu->addAction(label);
            action->setCheckable(true);
            action->setChecked(value == "1" || value.compare("true", Qt::CaseInsensitive) == 0);
            connect(action, &QAction::toggled, this, [this, discId, option, label](bool checked) {
                storeGameOptionValue(discId, option, checked ? QStringLiteral("1") : QStringLiteral("0"));
                gameEnhancementChanged(label);
            });
        }
        else if (type == "choice")
        {
            auto* submenu = m_gameEnhancementsMenu->addMenu(label + "  ·  " + optionDisplayValue(option, value));
            auto* group = new QActionGroup(submenu);
            group->setExclusive(true);
            const QJsonArray values = option.value("values").toArray();
            for (const QJsonValue& choiceValue : values)
            {
                const QJsonObject choice = choiceValue.toObject();
                const QString choiceLabel = choice.value("label").toString();
                const QString choiceData = choice.value("value").toVariant().toString();
                auto* action = submenu->addAction(choiceLabel);
                action->setCheckable(true);
                action->setChecked(choiceData == value);
                group->addAction(action);
                connect(action, &QAction::triggered, this,
                        [this, discId, option, choiceData, label] {
                            storeGameOptionValue(discId, option, choiceData);
                            updateGameEnhancementsMenu();
                            gameEnhancementChanged(label);
                        });
            }
        }
        else if (type == "number")
        {
            auto* action = m_gameEnhancementsMenu->addAction(
                label + QStringLiteral("…    ") + optionDisplayValue(option, value));
            connect(action, &QAction::triggered, this, [this, discId, option, label, value] {
                const double minimum = option.value("min").toDouble(0.0);
                const double maximum = option.value("max").toDouble(1000.0);
                const double suggested = option.value("suggested").toDouble(minimum);
                const double initial = value.isEmpty() ? suggested : value.toDouble();
                bool ok = false;
                const double result = QInputDialog::getDouble(
                    this, label, label + QStringLiteral(":"), initial, minimum, maximum,
                    option.value("decimals").toInt(0), &ok,
                    Qt::WindowFlags(), option.value("step").toDouble(1.0));
                if (!ok)
                    return;
                storeGameOptionValue(discId, option, QString::number(result, 'f', option.value("decimals").toInt(0)));
                updateGameEnhancementsMenu();
                gameEnhancementChanged(label);
            });
        }
        else if (type == "path")
        {
            const QString display = value.isEmpty() ? option.value("empty_label").toString("Not selected")
                                                    : QFileInfo(value).fileName();
            auto* action = m_gameEnhancementsMenu->addAction(label + QStringLiteral("…    ") + display);
            connect(action, &QAction::triggered, this, [this, discId, option, label, value] {
                const QString start = value.isEmpty() ? QDir::homePath() : value;
                const QString path = QFileDialog::getExistingDirectory(
                    this, label, start, QFileDialog::ShowDirsOnly | kPortableFileDialogOptions);
                if (path.isEmpty())
                    return;
                storeGameOptionValue(discId, option, QDir::cleanPath(path));
                updateGameEnhancementsMenu();
                gameEnhancementChanged(label);
            });
        }
    }

    m_gameEnhancementsMenu->addSeparator();
    auto* reset = m_gameEnhancementsMenu->addAction("Reset Game Enhancements");
    connect(reset, &QAction::triggered, this, [this, discId] {
        QSettings settings("GekkoAOT", "GekkoAOT");
        settings.remove(QStringLiteral("gameEnhancements/%1").arg(discId));
        updateGameEnhancementsMenu();
        gameEnhancementChanged("Game enhancements");
    });

    if (m_controller.isRunning())
    {
        auto* note = m_gameEnhancementsMenu->addAction("Changes apply after restarting the game");
        note->setEnabled(false);
    }
}

qreal MainWindow::effectiveDisplayScale() const
{
    const QScreen* current = m_gameViewport ? m_gameViewport->screen() : screen();
    if (!current)
        return 1.0;

    // Qt 6 works in device-independent pixels. Keep fractional values instead
    // of rounding 125/150/175% to an integer. On XWayland, depending on the
    // compositor, either DPR or logical DPI may carry the fractional scale, so
    // use the larger signal rather than multiplying them.
    const qreal dpr = current->devicePixelRatio();
    const qreal dpiScale = current->logicalDotsPerInch() / 96.0;
    return qBound<qreal>(1.0, qMax(dpr, dpiScale), 4.0);
}

QString MainWindow::displaySessionDescription() const
{
    const QString session = qEnvironmentVariable("XDG_SESSION_TYPE").toLower();
    const QString qpa = QGuiApplication::platformName();
    const qreal scale = effectiveDisplayScale();
    const QString scaleText = QString::number(scale * 100.0, 'f', 0);

    if (session == "wayland")
    {
        if (qpa == "xcb")
            return QString("Wayland session · embedded through XWayland · Qt scale %1%").arg(scaleText);
        return QString("Native Wayland session · external runtime window · Qt scale %1%").arg(scaleText);
    }
    if (qpa == "xcb" || session == "x11")
        return QString("X11 session · native embedded X11 · Qt scale %1%").arg(scaleText);
    return QString("Display backend: %1 · Qt scale %2%").arg(qpa, scaleText);
}

void MainWindow::showGraphicsConfig()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Graphics Settings");
    dialog.setModal(true);
    dialog.resize(520, 360);

    auto* outer = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout();

    auto* backend = new QComboBox(&dialog);
    backend->addItem("Vulkan", "Vulkan");
    backend->addItem("OpenGL", "OGL");
    backend->setCurrentIndex(comboIndexForData(backend, m_graphicsBackend));

    auto* resolution = new QComboBox(&dialog);
    resolution->addItem("Native (640×528)", "640x528");
    resolution->addItem("2× (1280×720)", "1280x720");
    resolution->addItem("3× (1920×1080)", "1920x1080");
    resolution->addItem("4× (2560×1440)", "2560x1440");
    resolution->addItem("6× (3840×2160)", "3840x2160");
    resolution->addItem("8× (5120×2880)", "5120x2880");
    resolution->addItem("12× (7680×4320)", "7680x4320");
    resolution->setCurrentIndex(comboIndexForData(resolution, m_resolution));

    auto* aspect = new QComboBox(&dialog);
    aspect->addItem("Auto", "auto");
    aspect->addItem("4:3", "4:3");
    aspect->addItem("16:9", "16:9");
    aspect->addItem("Stretch to window", "stretch");
    aspect->setCurrentIndex(comboIndexForData(aspect, m_aspectMode));

    auto* fpsLimit = new QSlider(Qt::Horizontal, &dialog);
    fpsLimit->setRange(30, 360);
    fpsLimit->setSingleStep(1);
    fpsLimit->setPageStep(10);
    fpsLimit->setTickInterval(30);
    fpsLimit->setTickPosition(QSlider::TicksBelow);
    fpsLimit->setValue(m_fpsLimit);
    auto* fpsValue = new QLabel(QString::number(m_fpsLimit) + " FPS", &dialog);
    fpsValue->setMinimumWidth(68);
    fpsValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* fpsRow = new QWidget(&dialog);
    auto* fpsRowLayout = new QHBoxLayout(fpsRow);
    fpsRowLayout->setContentsMargins(0, 0, 0, 0);
    fpsRowLayout->addWidget(fpsLimit, 1);
    fpsRowLayout->addWidget(fpsValue);
    connect(fpsLimit, &QSlider::valueChanged, &dialog, [fpsValue](int value) {
        fpsValue->setText(QString::number(value) + " FPS");
    });

    auto* fullscreen = new QCheckBox("Start in fullscreen", &dialog);
    fullscreen->setChecked(m_fullscreen);
    auto* showFps = new QCheckBox("Show performance HUD (VPS / FPS / Speed)", &dialog);
    showFps->setChecked(m_showFps);

    form->addRow("Backend", backend);
    form->addRow("Internal resolution", resolution);
    form->addRow("Aspect ratio", aspect);
    form->addRow("FPS limit", fpsRow);
    form->addRow(QString{}, fullscreen);
    form->addRow(QString{}, showFps);
    outer->addLayout(form);

    auto* hint = mutedLabel(
        displaySessionDescription() +
            "\nFPS limit controls host presentation/interpolation only. AOT CPU execution stays "
            "uncapped while VI/timebase/DEC/DSP/AI remain on the realtime 1.0x hardware clock. "
            "The HUD shows VPS, unique guest FPS, presented FPS, interpolated FPS and timer Speed; "
            "backend and resolution changes apply on the next launch.",
        &dialog);
    hint->setWordWrap(true);
    outer->addWidget(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    m_graphicsBackend = backend->currentData().toString();
    m_resolution = resolution->currentData().toString();
    m_aspectMode = aspect->currentData().toString();
    m_windowSystem = "auto";
    m_fullscreen = fullscreen->isChecked();
    m_showFps = showFps->isChecked();
    m_fpsLimit = fpsLimit->value();
    if (g_perfHud)
        g_perfHud->setVisible(m_showFps && m_runtimeStarted);
    saveSettings();

    QString error;
    if (!writeFrontendConfig(&error))
    {
        m_log->appendPlainText("[gekkoaot] " + error + "\n");
        setAdvancedVisible(true);
        setLogVisible(true);
        return;
    }
    statusBar()->showMessage("Graphics settings saved", 2500);
}

void MainWindow::showInputConfig()
{
    QDialog dialog(this);
    dialog.setWindowTitle("Input Manager — SDL3 native mapping");
    dialog.setModal(true);
    dialog.resize(900, 720);

    const QString profileFile = QDir(frontendUserDir()).filePath("input.ini");
    QVector<InputPortProfile> profiles = loadInputProfiles(profileFile);
    const QStringList detectedDevices = detectHostInputDevices();

    auto* outer = new QVBoxLayout(&dialog);
    auto* hint = mutedLabel(
        "Native SDL3 input. Auto combines the keyboard with the first matching gamepad/joystick. "
        "Keyboard, Gamepad and Joystick can be forced per port. Device match is optional and may "
        "contain any part of the device name; leaving it empty accepts any device.",
        &dialog);
    hint->setWordWrap(true);
    outer->addWidget(hint);

    struct DigitalWidgets
    {
        QKeySequenceEdit* key = nullptr;
        QComboBox* button = nullptr;
        QSpinBox* joystick = nullptr;
    };
    struct AxisWidgets
    {
        QKeySequenceEdit* negative = nullptr;
        QKeySequenceEdit* positive = nullptr;
        QComboBox* axis = nullptr;
        QSpinBox* joystick = nullptr;
        QCheckBox* invert = nullptr;
        QSpinBox* deadzone = nullptr;
    };
    struct TriggerWidgets
    {
        QKeySequenceEdit* key = nullptr;
        QComboBox* axis = nullptr;
        QSpinBox* joystick = nullptr;
    };
    struct PortWidgets
    {
        QComboBox* source = nullptr;
        QComboBox* device = nullptr;
        QMap<QString, DigitalWidgets> digital;
        QMap<QString, AxisWidgets> axes;
        QMap<QString, TriggerWidgets> triggers;
    };

    const QList<QPair<QString, QString>> digitalControls = {
        {"A", "a"}, {"B", "b"}, {"X", "x"}, {"Y", "y"}, {"Start", "start"},
        {"Z", "z"}, {"L digital", "l"}, {"R digital", "r"},
        {"D-Pad Up", "dpad_up"}, {"D-Pad Down", "dpad_down"},
        {"D-Pad Left", "dpad_left"}, {"D-Pad Right", "dpad_right"},
    };
    const QList<QPair<QString, QString>> gamepadButtons = {
        {"None", ""}, {"South / A", "south"}, {"East / B", "east"},
        {"West / X", "west"}, {"North / Y", "north"}, {"Start", "start"},
        {"Back", "back"}, {"Guide", "guide"}, {"Left Shoulder", "left_shoulder"},
        {"Right Shoulder", "right_shoulder"}, {"Left Stick", "left_stick"},
        {"Right Stick", "right_stick"}, {"D-Pad Up", "dpad_up"},
        {"D-Pad Down", "dpad_down"}, {"D-Pad Left", "dpad_left"},
        {"D-Pad Right", "dpad_right"},
    };
    const QList<QPair<QString, QString>> axes = {
        {"None", ""}, {"Left X", "leftx"}, {"Left Y", "lefty"},
        {"Right X", "rightx"}, {"Right Y", "righty"},
        {"Left Trigger", "left_trigger"}, {"Right Trigger", "right_trigger"},
    };
    const QList<QPair<QString, QString>> axisControls = {
        {"Main Stick X", "main_x"}, {"Main Stick Y", "main_y"},
        {"C-Stick X", "c_x"}, {"C-Stick Y", "c_y"},
    };
    const QList<QPair<QString, QString>> triggerControls = {
        {"L analog", "l_analog"}, {"R analog", "r_analog"},
    };

    const auto setComboData = [](QComboBox* combo, const QString& value) {
        const int index = combo->findData(value);
        if (index >= 0)
            combo->setCurrentIndex(index);
    };
    const auto sequence = [](const QString& value) {
        return QKeySequence::fromString(value, QKeySequence::PortableText);
    };

    auto* tabs = new QTabWidget(&dialog);
    QVector<PortWidgets> widgets(4);

    const auto loadWidgets = [&](int port, const InputPortProfile& profile) {
        PortWidgets& w = widgets[port];
        setComboData(w.source, profile.source);
        w.device->setCurrentText(profile.device);
        for (const auto& control : digitalControls)
        {
            auto& d = w.digital[control.second];
            d.key->setKeySequence(sequence(profile.values.value(control.second + "_key")));
            setComboData(d.button, profile.values.value(control.second + "_button"));
            d.joystick->setValue(profile.values.value(control.second + "_joy_button", "-1").toInt());
        }
        for (const auto& control : axisControls)
        {
            auto& a = w.axes[control.second];
            a.negative->setKeySequence(sequence(profile.values.value(control.second + "_negative_key")));
            a.positive->setKeySequence(sequence(profile.values.value(control.second + "_positive_key")));
            setComboData(a.axis, profile.values.value(control.second + "_axis"));
            a.joystick->setValue(profile.values.value(control.second + "_joy_axis", "-1").toInt());
            a.invert->setChecked(profile.values.value(control.second + "_invert") == "true");
            a.deadzone->setValue(profile.values.value(control.second + "_deadzone", "4096").toInt());
        }
        for (const auto& control : triggerControls)
        {
            auto& t = w.triggers[control.second];
            t.key->setKeySequence(sequence(profile.values.value(control.second + "_key")));
            setComboData(t.axis, profile.values.value(control.second + "_axis"));
            t.joystick->setValue(profile.values.value(control.second + "_joy_axis", "-1").toInt());
        }
    };

    for (int port = 0; port < 4; ++port)
    {
        auto* scroll = new QScrollArea(tabs);
        scroll->setWidgetResizable(true);
        auto* page = new QWidget(scroll);
        auto* pageLayout = new QVBoxLayout(page);
        PortWidgets& w = widgets[port];

        auto* deviceBox = new QGroupBox("Device", page);
        auto* deviceForm = new QFormLayout(deviceBox);
        w.source = new QComboBox(deviceBox);
        w.source->addItem("Auto — keyboard + first compatible device", "auto");
        w.source->addItem("Keyboard only", "keyboard");
        w.source->addItem("Gamepad only", "gamepad");
        w.source->addItem("Generic joystick only", "joystick");
        w.source->addItem("Disabled", "disabled");
        deviceForm->addRow("Source", w.source);
        w.device = new QComboBox(deviceBox);
        w.device->setEditable(true);
        w.device->addItem("", "");
        for (const QString& device : detectedDevices)
            w.device->addItem(device, device);
        w.device->lineEdit()->setPlaceholderText("Any device (or type a name substring)");
        deviceForm->addRow("Device match", w.device);
        auto* deviceNote = mutedLabel(
            detectedDevices.isEmpty()
                ? "No host device names were discovered by the GUI. Auto/Any still works because SDL3 performs runtime detection."
                : QString("Detected by host: %1").arg(detectedDevices.join(", ")),
            deviceBox);
        deviceNote->setWordWrap(true);
        deviceForm->addRow(deviceNote);
        pageLayout->addWidget(deviceBox);

        auto* digitalBox = new QGroupBox("Buttons", page);
        auto* digitalGrid = new QGridLayout(digitalBox);
        digitalGrid->addWidget(new QLabel("GameCube"), 0, 0);
        digitalGrid->addWidget(new QLabel("Keyboard"), 0, 1);
        digitalGrid->addWidget(new QLabel("SDL gamepad"), 0, 2);
        digitalGrid->addWidget(new QLabel("Joystick button"), 0, 3);
        int row = 1;
        for (const auto& control : digitalControls)
        {
            DigitalWidgets d;
            d.key = new QKeySequenceEdit(digitalBox);
            d.key->setMaximumSequenceLength(1);
            d.button = new QComboBox(digitalBox);
            for (const auto& button : gamepadButtons)
                d.button->addItem(button.first, button.second);
            d.joystick = new QSpinBox(digitalBox);
            d.joystick->setRange(-1, 63);
            d.joystick->setSpecialValueText("None");
            digitalGrid->addWidget(new QLabel(control.first, digitalBox), row, 0);
            digitalGrid->addWidget(d.key, row, 1);
            digitalGrid->addWidget(d.button, row, 2);
            digitalGrid->addWidget(d.joystick, row, 3);
            w.digital.insert(control.second, d);
            ++row;
        }
        pageLayout->addWidget(digitalBox);

        auto* analogBox = new QGroupBox("Analog sticks", page);
        auto* analogGrid = new QGridLayout(analogBox);
        analogGrid->addWidget(new QLabel("Axis"), 0, 0);
        analogGrid->addWidget(new QLabel("Keyboard -"), 0, 1);
        analogGrid->addWidget(new QLabel("Keyboard +"), 0, 2);
        analogGrid->addWidget(new QLabel("SDL axis"), 0, 3);
        analogGrid->addWidget(new QLabel("Joy axis"), 0, 4);
        analogGrid->addWidget(new QLabel("Invert"), 0, 5);
        analogGrid->addWidget(new QLabel("Deadzone"), 0, 6);
        row = 1;
        for (const auto& control : axisControls)
        {
            AxisWidgets a;
            a.negative = new QKeySequenceEdit(analogBox);
            a.positive = new QKeySequenceEdit(analogBox);
            a.negative->setMaximumSequenceLength(1);
            a.positive->setMaximumSequenceLength(1);
            a.axis = new QComboBox(analogBox);
            for (const auto& axis : axes)
                a.axis->addItem(axis.first, axis.second);
            a.joystick = new QSpinBox(analogBox);
            a.joystick->setRange(-1, 31);
            a.joystick->setSpecialValueText("None");
            a.invert = new QCheckBox(analogBox);
            a.deadzone = new QSpinBox(analogBox);
            a.deadzone->setRange(0, 30000);
            a.deadzone->setSingleStep(512);
            analogGrid->addWidget(new QLabel(control.first, analogBox), row, 0);
            analogGrid->addWidget(a.negative, row, 1);
            analogGrid->addWidget(a.positive, row, 2);
            analogGrid->addWidget(a.axis, row, 3);
            analogGrid->addWidget(a.joystick, row, 4);
            analogGrid->addWidget(a.invert, row, 5, Qt::AlignCenter);
            analogGrid->addWidget(a.deadzone, row, 6);
            w.axes.insert(control.second, a);
            ++row;
        }
        pageLayout->addWidget(analogBox);

        auto* triggerBox = new QGroupBox("Analog triggers", page);
        auto* triggerGrid = new QGridLayout(triggerBox);
        triggerGrid->addWidget(new QLabel("Trigger"), 0, 0);
        triggerGrid->addWidget(new QLabel("Keyboard"), 0, 1);
        triggerGrid->addWidget(new QLabel("SDL axis"), 0, 2);
        triggerGrid->addWidget(new QLabel("Joy axis"), 0, 3);
        row = 1;
        for (const auto& control : triggerControls)
        {
            TriggerWidgets t;
            t.key = new QKeySequenceEdit(triggerBox);
            t.key->setMaximumSequenceLength(1);
            t.axis = new QComboBox(triggerBox);
            for (const auto& axis : axes)
                t.axis->addItem(axis.first, axis.second);
            t.joystick = new QSpinBox(triggerBox);
            t.joystick->setRange(-1, 31);
            t.joystick->setSpecialValueText("None");
            triggerGrid->addWidget(new QLabel(control.first, triggerBox), row, 0);
            triggerGrid->addWidget(t.key, row, 1);
            triggerGrid->addWidget(t.axis, row, 2);
            triggerGrid->addWidget(t.joystick, row, 3);
            w.triggers.insert(control.second, t);
            ++row;
        }
        pageLayout->addWidget(triggerBox);

        auto* reset = new QPushButton("Reset this port to defaults", page);
        connect(reset, &QPushButton::clicked, &dialog, [&, port] {
            profiles[port] = defaultInputPortProfile(port);
            loadWidgets(port, profiles[port]);
        });
        pageLayout->addWidget(reset, 0, Qt::AlignLeft);
        pageLayout->addStretch();
        scroll->setWidget(page);
        tabs->addTab(scroll, QString("Port %1").arg(port + 1));
        loadWidgets(port, profiles[port]);
    }
    outer->addWidget(tabs, 1);

    auto* footer = mutedLabel(
        "The runtime still uses SDL3 for hotplug, standardized gamepad mappings, generic joystick fallback and rumble. "
        "The GUI only stores portable bindings; you never need to type SDL/0/... identifiers.",
        &dialog);
    footer->setWordWrap(true);
    outer->addWidget(footer);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return;

    for (int port = 0; port < 4; ++port)
    {
        PortWidgets& w = widgets[port];
        InputPortProfile profile;
        profile.source = w.source->currentData().toString();
        profile.device = w.device->currentText().trimmed();
        for (const auto& control : digitalControls)
        {
            const auto d = w.digital.value(control.second);
            profile.values[control.second + "_key"] =
                d.key->keySequence().toString(QKeySequence::PortableText);
            profile.values[control.second + "_button"] = d.button->currentData().toString();
            profile.values[control.second + "_joy_button"] = QString::number(d.joystick->value());
        }
        for (const auto& control : axisControls)
        {
            const auto a = w.axes.value(control.second);
            profile.values[control.second + "_negative_key"] =
                a.negative->keySequence().toString(QKeySequence::PortableText);
            profile.values[control.second + "_positive_key"] =
                a.positive->keySequence().toString(QKeySequence::PortableText);
            profile.values[control.second + "_axis"] = a.axis->currentData().toString();
            profile.values[control.second + "_joy_axis"] = QString::number(a.joystick->value());
            profile.values[control.second + "_invert"] = a.invert->isChecked() ? "true" : "false";
            profile.values[control.second + "_deadzone"] = QString::number(a.deadzone->value());
        }
        for (const auto& control : triggerControls)
        {
            const auto t = w.triggers.value(control.second);
            profile.values[control.second + "_key"] =
                t.key->keySequence().toString(QKeySequence::PortableText);
            profile.values[control.second + "_axis"] = t.axis->currentData().toString();
            profile.values[control.second + "_joy_axis"] = QString::number(t.joystick->value());
        }
        profiles[port] = profile;
    }

    QString error;
    if (!saveInputProfiles(profileFile, profiles, &error))
    {
        m_log->appendPlainText("[gekkoaot] " + error + "\n");
        setAdvancedVisible(true);
        setLogVisible(true);
        return;
    }

    // Keep portable SDL controller hints for the native runtime.
    m_controllers.clear();
    for (const InputPortProfile& profile : profiles)
    {
        if ((profile.source == "gamepad" || profile.source == "auto") && !profile.device.isEmpty())
            m_controllers.append("SDL/0/" + profile.device);
        else
            m_controllers.append(QString{});
    }
    saveSettings();
    if (!writeFrontendConfig(&error))
    {
        m_log->appendPlainText("[gekkoaot] " + error + "\n");
        setAdvancedVisible(true);
        setLogVisible(true);
        return;
    }
    statusBar()->showMessage("Native input profile saved — hotplug and auto-detection enabled", 3000);
}

BuildRequest MainWindow::requestFor(const QString& action) const
{
    BuildRequest request;
    request.action = action;
    request.isoPath = m_isoPath;
    request.gameSlug = slugForPath(m_isoPath);
    request.backend = m_backend->currentData().toString();
    request.graphicsBackend = m_graphicsBackend;
    request.profile = m_profile->currentData().toString();
    request.toolchain = m_toolchain->currentData().toString();
    request.llvmDir = m_llvmDir->text().trimmed();
    request.nativeAbi = m_nativeAbi->currentData().toString();
    request.jobs = m_jobs->value();
    request.runArgs = m_runArgs->text().trimmed();
    request.userDir = frontendUserDir();
    request.aspectMode = m_aspectMode;
    request.resolution = m_resolution;
    request.fullscreen = m_fullscreen;
    request.fpsLimit = m_fpsLimit;
    request.uiScale = QString::number(effectiveDisplayScale(), 'f', 3);
    request.inputConfig = QDir(frontendUserDir()).filePath("input.ini");
    request.gameEnvironment = gameEnhancementEnvironment();

    // Cross-process native child embedding exists on X11, not Wayland. On a
    // normal GNOME/KDE Wayland desktop DISPLAY points at XWayland, and main.cpp
    // selects Qt's xcb plugin so the same GameView XID can be presented by
    // Vulkan. If XWayland is unavailable, keep the GUI native Wayland and let
    // Aurora creates its own top-level on pure Wayland; on XWayland the bridge
    // reparents its SDL window into the Qt-owned GameView child.
    const bool canEmbedX11 =
        QGuiApplication::platformName() == "xcb" && !qEnvironmentVariableIsEmpty("DISPLAY");
    if (canEmbedX11)
    {
        request.windowSystem = "x11";
        request.embedWindow = QString::number(static_cast<qulonglong>(m_gameViewport->winId()));
    }
    else if (qEnvironmentVariable("XDG_SESSION_TYPE").compare("wayland", Qt::CaseInsensitive) == 0)
    {
        request.windowSystem = "wayland";
        request.embedWindow.clear();
    }
    else
    {
        request.windowSystem = "x11";
        request.embedWindow.clear();
    }
    return request;
}

void MainWindow::launchAction(const QString& action)
{
    saveSettings();

    if (action != "tools" && action != "clean" &&
        (!QFileInfo::exists(m_isoPath) || !looksLikeDiscImage(m_isoPath)))
    {
        showIdleViewport("Choose a valid disc image first");
        statusBar()->showMessage("No valid game selected", 2500);
        return;
    }

    if (action == "pgo" && m_backend->currentData().toString() != "llvm")
    {
        statusBar()->showMessage("PGO training requires the LLVM AOT backend", 5000);
        m_log->appendPlainText("[gekkoaot] PGO training requires the LLVM AOT backend.\n");
        setAdvancedVisible(true);
        setLogVisible(true);
        return;
    }

    if (action == "run" || action == "pgo")
    {
        QString error;
        if (!writeFrontendConfig(&error))
        {
            m_log->appendPlainText("[gekkoaot] " + error + "\n");
            setAdvancedVisible(true);
            setLogVisible(true);
            return;
        }
    }

    m_activeAction = action;
    m_runtimeStarted = false;
    m_stopRequested = false;
    m_progressStage.clear();
    m_progressSubstage.clear();
    m_progressBuffer.clear();
    m_secondaryKind.clear();
    m_secondaryName.clear();
    m_secondaryIndex = 0;
    m_secondaryTotal = 0;
    m_lastProgress = 0;
    const QString gameTitle = gameTitleForPath(m_isoPath);
    if (action == "run")
        showLoader(gameTitle, "Preparing native runtime…", 3);
    else if (action == "pgo")
        showLoader(gameTitle, "Preparing PGO training…", 3);
    else if (action == "compile")
        showLoader(gameTitle, "Compiling native module…", 5);
    else if (action == "inspect")
        showLoader(gameTitle, "Reading disc executables…", 5);
    else if (action == "tools")
        showLoader("GekkoAOT", "Building engine…", 5);
    else if (action == "clean")
        showLoader("GekkoAOT", "Cleaning cache…", 25);

    m_controller.run(requestFor(action));
}

void MainWindow::setAdvancedVisible(bool visible)
{
    if (!m_advancedDialog)
        return;
    if (visible)
    {
        m_advancedDialog->show();
        m_advancedDialog->raise();
        m_advancedDialog->activateWindow();
    }
    else
    {
        m_advancedDialog->hide();
    }
    if (m_advancedAction && m_advancedAction->isChecked() != visible)
        m_advancedAction->setChecked(visible);
}

void MainWindow::setLogVisible(bool visible)
{
    if (visible)
        setAdvancedVisible(true);
    m_logPanel->setVisible(visible);
    if (m_showLogCheck && m_showLogCheck->isChecked() != visible)
        m_showLogCheck->setChecked(visible);
}

void MainWindow::toggleGameViewFullscreen()
{
    // The window manager is the source of truth. This intentionally does not
    // trust m_gameViewFullscreen: fullscreen transitions are asynchronous on
    // Wayland/XWayland and the cached flag can lag one key press behind.
    setGameViewFullscreen(!isFullScreen());
}

void MainWindow::setGameViewFullscreen(bool fullscreen)
{
    const bool actualFullscreen = isFullScreen();
    if (actualFullscreen == fullscreen)
    {
        m_gameViewFullscreen = actualFullscreen;
        if (m_fullscreenAction && m_fullscreenAction->isChecked() != actualFullscreen)
        {
            const QSignalBlocker blocker(m_fullscreenAction);
            m_fullscreenAction->setChecked(actualFullscreen);
        }
        return;
    }

    if (fullscreen)
    {
        m_wasMaximizedBeforeGameFullscreen = isMaximized();
        m_restoreAdvancedAfterGameFullscreen =
            m_advancedDialog && m_advancedDialog->isVisible();

        if (m_advancedDialog)
            m_advancedDialog->hide();
        menuBar()->hide();
        if (m_mainToolbar)
            m_mainToolbar->hide();
        statusBar()->hide();

        // Set the state explicitly as well as calling showFullScreen(). KDE's
        // XWayland path can otherwise leave showNormal() unable to clear a
        // fullscreen bit that arrived asynchronously from the compositor.
        setWindowState(windowState() | Qt::WindowFullScreen);
        showFullScreen();
    }
    else
    {
        setWindowState(windowState() & ~Qt::WindowFullScreen);
        showNormal();
        menuBar()->show();
        if (m_mainToolbar)
            m_mainToolbar->show();
        statusBar()->show();

        if (m_wasMaximizedBeforeGameFullscreen)
            QTimer::singleShot(0, this, [this] { showMaximized(); });

        if (m_restoreAdvancedAfterGameFullscreen)
        {
            m_restoreAdvancedAfterGameFullscreen = false;
            QTimer::singleShot(0, this, [this] { setAdvancedVisible(true); });
        }
    }

    // WindowStateChange is asynchronous. Re-sync once the compositor has
    // processed the request, then focus the GameView/loader without affecting
    // the background QProcess build.
    QTimer::singleShot(0, this, [this, fullscreen] {
        m_gameViewFullscreen = isFullScreen();
        if (m_fullscreenAction && m_fullscreenAction->isChecked() != m_gameViewFullscreen)
        {
            const QSignalBlocker blocker(m_fullscreenAction);
            m_fullscreenAction->setChecked(m_gameViewFullscreen);
        }
        if (fullscreen && isFullScreen())
        {
            if (m_runtimeStarted)
                m_gameViewport->setFocus(Qt::ShortcutFocusReason);
            else
                m_loaderOverlay->setFocus(Qt::ShortcutFocusReason);
        }
    });
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (!event || event->type() != QEvent::WindowStateChange)
        return;

    m_gameViewFullscreen = isFullScreen();
    if (m_fullscreenAction && m_fullscreenAction->isChecked() != m_gameViewFullscreen)
    {
        const QSignalBlocker blocker(m_fullscreenAction);
        m_fullscreenAction->setChecked(m_gameViewFullscreen);
    }

    // Also restore chrome if fullscreen was cleared by the WM rather than by
    // our own shortcut (for example KDE's fullscreen action).
    if (!m_gameViewFullscreen)
    {
        menuBar()->show();
        if (m_mainToolbar)
            m_mainToolbar->show();
        statusBar()->show();
    }
}

void MainWindow::showLoader(const QString& title, const QString& status, int progress)
{
    progress = qBound(0, progress, 100);
    if (!m_activeAction.isEmpty() && progress < m_lastProgress)
        progress = m_lastProgress;
    m_lastProgress = progress;

    m_loaderTitle->setText(title.isEmpty() ? QStringLiteral("GekkoAOT") : title);
    // Clear first so a shorter status cannot retain pixels from the previous
    // one on some XWayland/Qt backing-store combinations.
    m_loaderStatus->clear();
    m_loaderStatus->setText(status);
    m_loaderHint->setText("Build runs in the background · Advanced → Show Log");
    m_progress->setRange(0, 100);
    m_progress->setValue(progress);
    m_progress->setFormat(QString::number(progress) + "%");
    m_loaderOverlay->show();
    m_loaderOverlay->raise();
    m_viewStack->setCurrentWidget(m_loaderOverlay);
    m_loaderOverlay->update();
}

void MainWindow::showGameViewport()
{
    m_progress->setValue(100);
    m_loaderOverlay->hide();
    m_gameViewport->show();
    m_gameViewport->setFocus(Qt::OtherFocusReason);
    m_viewStack->setCurrentWidget(m_gameViewport);
    m_runtimeStarted = true;
    if (g_perfHud)
    {
        g_perfHud->setVisible(m_showFps);
        g_perfHud->raise();
    }
}

void MainWindow::showIdleViewport(const QString& message)
{
    if (g_perfHud)
        g_perfHud->hide();
    updateLoaderBanner();
    const QFileInfo info(m_isoPath);
    m_loaderTitle->setText(info.isFile() ? gameTitleForPath(m_isoPath) : QStringLiteral("GekkoAOT"));
    m_loaderStatus->setText(message.isEmpty() ? QStringLiteral("Open a GameCube/Wii disc image to start")
                                               : message);
    m_loaderHint->setText(info.isFile() ? QStringLiteral("Play") : QStringLiteral("File → Open Game"));
    m_lastProgress = 0;
    m_progressStage.clear();
    m_progressSubstage.clear();
    m_secondaryKind.clear();
    m_secondaryName.clear();
    m_secondaryIndex = 0;
    m_secondaryTotal = 0;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setFormat("0%");
    m_loaderOverlay->show();
    m_loaderOverlay->raise();
    m_viewStack->setCurrentWidget(m_loaderOverlay);
}

void MainWindow::updateStatusFromOutput(const QString& text)
{
    // QProcess output is asynchronous and chunks do not necessarily end on a
    // line boundary. Keep a small rolling buffer so progress markers and Ninja
    // counters are still recognized when a line is split between signals.
    m_progressBuffer += text;
    if (m_progressBuffer.size() > 32768)
        m_progressBuffer = m_progressBuffer.right(32768);

    const QString gameTitle = gameTitleForPath(m_isoPath);

    static const QRegularExpression stageRe(R"(GEKKOAOT_STAGE=([A-Za-z0-9_-]+))");
    static const QRegularExpression substageRe(R"(GEKKOAOT_SUBSTAGE=([A-Za-z0-9_-]+))");
    static const QRegularExpression markerRe(R"(GEKKOAOT_PROGRESS=(\d+)\|([^\r\n]+))");
    static const QRegularExpression perfRe(
        R"(GEKKOAOT_PERF_V49=1 vps=([0-9.]+) nominal_vps=([0-9.]+) guest_fps=([0-9.]+) present_fps=([0-9.]+) interp_fps=([0-9.]+) speed=([0-9.]+) target=(\d+) drops=(\d+))");
    static const QRegularExpression secondaryItemRe(
        R"(GEKKOAOT_SECONDARY_ITEM=1\s+kind=([A-Za-z0-9_-]+)\s+index=(\d+)\s+total=(\d+)\s+source=\"([^\"]+)\")"
    );
    // Compatibility with the first overlay implementation: it already emitted
    // this marker before the generic SECONDARY_ITEM marker existed.
    static const QRegularExpression relOverlayRe(
        R"(GEKKOAOT_REL_OVERLAY_BUILD=1\s+index=(\d+)\s+total=(\d+)\s+source=\"([^\"]+)\")"
    );
    // Batch REL mode (unique module ids) is reported by DolRecomp itself.
    static const QRegularExpression relBatchRe(
        R"(^REL\s+(\d+)/(\d+):\s+([^\r\n]+\.rel)\s*$)",
        QRegularExpression::MultilineOption
    );

    QRegularExpressionMatch match;
    QRegularExpressionMatch perfMatch;
    auto perfMatches = perfRe.globalMatch(m_progressBuffer);
    while (perfMatches.hasNext())
        perfMatch = perfMatches.next();
    if (perfMatch.hasMatch() && g_perfHud)
    {
        const double vps = perfMatch.captured(1).toDouble();
        const double nominalVps = perfMatch.captured(2).toDouble();
        const double guestFps = perfMatch.captured(3).toDouble();
        const double presentFps = perfMatch.captured(4).toDouble();
        const double interpFps = perfMatch.captured(5).toDouble();
        const double speed = perfMatch.captured(6).toDouble();
        const int target = perfMatch.captured(7).toInt();
        const qulonglong drops = perfMatch.captured(8).toULongLong();
        g_perfHud->setText(
            QString("VPS %1/%2  ·  G-FPS %3  ·  FPS %4  ·  I-FPS %5  ·  Speed %6%  ·  Target %7  ·  Drop %8")
                .arg(vps, 0, 'f', 1).arg(nominalVps, 0, 'f', 1)
                .arg(guestFps, 0, 'f', 1).arg(presentFps, 0, 'f', 1)
                .arg(interpFps, 0, 'f', 1).arg(speed, 0, 'f', 1)
                .arg(target).arg(drops));
        g_perfHud->adjustSize();
        g_perfHud->move(12, 12);
        g_perfHud->setVisible(m_showFps && m_runtimeStarted);
        g_perfHud->raise();
    }
    auto stageMatches = stageRe.globalMatch(m_progressBuffer);
    while (stageMatches.hasNext())
        match = stageMatches.next();
    if (match.hasMatch())
        m_progressStage = match.captured(1);

    match = QRegularExpressionMatch();
    auto substageMatches = substageRe.globalMatch(m_progressBuffer);
    while (substageMatches.hasNext())
        match = substageMatches.next();
    if (match.hasMatch())
        m_progressSubstage = match.captured(1);

    // Explicit controller markers are the source of truth at phase boundaries.
    // Only react when this QProcess delivery actually contains a new marker;
    // otherwise an old marker in the rolling buffer would overwrite a newer
    // detailed Ninja/chunk status on every subsequent output burst.
    QRegularExpressionMatch lastMarker;
    auto markerMatches = markerRe.globalMatch(text);
    while (markerMatches.hasNext())
        lastMarker = markerMatches.next();
    if (lastMarker.hasMatch())
    {
        const int value = lastMarker.captured(1).toInt();
        showLoader(gameTitle, lastMarker.captured(2).trimmed(), value);
    }

    // Secondary executables are long enough that a single overall percentage
    // is not useful. Keep the current REL/ELF/DOL identity so the loader can
    // show both file-level and LLVM-chunk-level progress.
    QRegularExpressionMatch secondaryItem;
    auto secondaryMatches = secondaryItemRe.globalMatch(text);
    while (secondaryMatches.hasNext())
        secondaryItem = secondaryMatches.next();

    if (!secondaryItem.hasMatch())
    {
        QRegularExpressionMatch overlay;
        auto overlayMatches = relOverlayRe.globalMatch(text);
        while (overlayMatches.hasNext())
            overlay = overlayMatches.next();
        if (overlay.hasMatch())
        {
            m_secondaryKind = QStringLiteral("REL");
            m_secondaryIndex = overlay.captured(1).toInt();
            m_secondaryTotal = qMax(1, overlay.captured(2).toInt());
            m_secondaryName = QFileInfo(overlay.captured(3)).fileName();
            const int progress = qBound(93, 93 + ((m_secondaryIndex - 1) * 6 / m_secondaryTotal), 99);
            showLoader(gameTitle,
                       QString("REL %1/%2 — %3 · preparing LLVM AOT")
                           .arg(m_secondaryIndex).arg(m_secondaryTotal).arg(m_secondaryName),
                       progress);
        }
    }
    else
    {
        m_secondaryKind = secondaryItem.captured(1).toUpper();
        m_secondaryIndex = secondaryItem.captured(2).toInt();
        m_secondaryTotal = qMax(1, secondaryItem.captured(3).toInt());
        m_secondaryName = QFileInfo(secondaryItem.captured(4)).fileName();
        const int progress = qBound(93, 93 + ((m_secondaryIndex - 1) * 6 / m_secondaryTotal), 99);
        showLoader(gameTitle,
                   QString("%1 %2/%3 — %4 · preparing LLVM AOT")
                       .arg(m_secondaryKind).arg(m_secondaryIndex).arg(m_secondaryTotal).arg(m_secondaryName),
                   progress);
    }

    // For non-overlay REL batches, DolRecomp tells us which REL in the batch is
    // being decoded. This also keeps the GUI useful for games without duplicate
    // module IDs.
    QRegularExpressionMatch relBatch;
    auto relBatchMatches = relBatchRe.globalMatch(text);
    while (relBatchMatches.hasNext())
        relBatch = relBatchMatches.next();
    if (relBatch.hasMatch() && m_secondaryKind.isEmpty())
    {
        m_secondaryKind = QStringLiteral("REL");
        m_secondaryIndex = relBatch.captured(1).toInt();
        m_secondaryTotal = qMax(1, relBatch.captured(2).toInt());
        m_secondaryName = QFileInfo(relBatch.captured(3).trimmed()).fileName();
    }

    // DolRecomp's LLVM backend is already split into independently hashed
    // chunks. Surface it like a shader cache, including exact block progress.
    static const QRegularExpression llvmChunkRe(
        R"(\[(\d+)/(\d+)\]\s+(Reusing cached|Emitting)\s+LLVM object)"
    );
    QRegularExpressionMatch lastChunk;
    if (text.contains("LLVM object"))
    {
        auto chunkMatches = llvmChunkRe.globalMatch(m_progressBuffer);
        while (chunkMatches.hasNext())
            lastChunk = chunkMatches.next();
    }

    if (lastChunk.hasMatch())
    {
        m_progressStage = "aot";
        const int current = lastChunk.captured(1).toInt();
        const int total = qMax(1, lastChunk.captured(2).toInt());
        const bool cached = lastChunk.captured(3).startsWith("Reusing");
        int progress = qBound(50, 50 + (current * 40 / total), 90);
        const QString state = cached ? QStringLiteral("cached LLVM")
                                     : QStringLiteral("LLVM");
        QString status;
        if (!m_secondaryKind.isEmpty() && m_secondaryIndex > 0 && m_secondaryTotal > 0)
        {
            // Weight each secondary executable equally in the final 93..99%
            // section while retaining the exact LLVM object counter in text.
            const qint64 done = qint64(m_secondaryIndex - 1) * total + current;
            const qint64 all = qint64(m_secondaryTotal) * total;
            progress = qBound(93, 93 + int(done * 6 / qMax<qint64>(1, all)), 99);
            status = QString("%1 %2/%3 — %4 · %5 %6/%7")
                         .arg(m_secondaryKind)
                         .arg(m_secondaryIndex)
                         .arg(m_secondaryTotal)
                         .arg(m_secondaryName)
                         .arg(state)
                         .arg(current)
                         .arg(total);
        }
        else
        {
            status = QString("%1 — %2/%3").arg(cached ? QStringLiteral("Reusing cached AOT blocks")
                                                       : QStringLiteral("Compiling AOT blocks"))
                                               .arg(current).arg(total);
        }
        showLoader(gameTitle, status, progress);
    }
    else
    {
        // Ninja gives real item counts while the compiler/runtime and final
        // native module are being built. Map those counters into the overall
        // one-click Play progress instead of displaying a frozen phase value.
        static const QRegularExpression ninjaRe(R"(\[(\d+)/(\d+)\]\s+([^\r\n]+))");
        QRegularExpressionMatch lastNinja;
        auto ninjaMatches = ninjaRe.globalMatch(text);
        while (ninjaMatches.hasNext())
            lastNinja = ninjaMatches.next();

        if (lastNinja.hasMatch())
        {
            const int current = lastNinja.captured(1).toInt();
            const int total = qMax(1, lastNinja.captured(2).toInt());
            const QString item = lastNinja.captured(3).trimmed();
            int base = -1;
            int span = 0;
            QString label;

            if (m_progressSubstage == "dolrecomp-build")
            {
                base = 10;
                span = 8;
                label = "Building AOT compiler";
            }
            else if (m_progressSubstage == "aurora-build")
            {
                base = 22;
                span = 16;
                label = "Building runtime";
            }
            else if (m_progressStage == "aot")
            {
                base = 91;
                span = 7;
                label = "Linking native module";
            }

            if (base >= 0)
            {
                const int progress = qBound(base, base + current * span / total, base + span);
                QString detail = item;
                if (detail.size() > 72)
                    detail = detail.left(69) + "…";
                showLoader(gameTitle,
                           QString("%1 — %2/%3 · %4")
                               .arg(label).arg(current).arg(total).arg(detail),
                           progress);
            }
        }
    }

    // Runtime visibility has priority over build/link messages. The native
    // controller emits GEKKOAOT_STAGE=runtime immediately before native-run.
    if (text.contains("GEKKOAOT_STAGE=runtime") ||
        text.contains("[2/6] PGO training has started"))
    {
        m_progressStage = "runtime";
        m_secondaryKind.clear();
        m_secondaryName.clear();
        m_secondaryIndex = 0;
        m_secondaryTotal = 0;
        showLoader(gameTitle,
                   m_activeAction == "pgo" ? QStringLiteral("PGO training — game running")
                                            : QStringLiteral("Starting game"),
                   100);
        QTimer::singleShot(25, this, &MainWindow::showGameViewport);
    }
    else if (text.contains("[3/6] Collecting raw profiles"))
        showLoader(gameTitle, "Collecting PGO profiles", 100);
    else if (text.contains("[4/6] Merging profiles"))
        showLoader(gameTitle, "Merging PGO profiles", 100);
    else if (text.contains("[5/6] Validating the merged profile"))
        showLoader(gameTitle, "Validating PGO profile", 100);
    else if (text.contains("[6/6] Building the PGO module"))
        showLoader(gameTitle, "Building optimized PGO module", 100);
    else if (text.contains("GEKKOAOT_ENGINE_CACHE_HIT=1"))
        showLoader(gameTitle, "Compiler/runtime cache ready", 38);
    else if (text.contains("==> executable cache hit"))
    {
        updateLoaderBanner();
        updateGameMetadata();
        showLoader(gameTitle, "Disc executable cache ready", 47);
    }
    else if (text.contains("==> executable cache ready"))
    {
        updateLoaderBanner();
        updateGameMetadata();
        showLoader(gameTitle, "Disc executables ready", 47);
    }
    else if (text.contains("cache hit:"))
        showLoader(gameTitle, "Native module cache hit", 97);
    else if (text.contains("built module:"))
        showLoader(gameTitle, "Native module ready", 98);

    statusBar()->showMessage(m_loaderStatus->text());
}

void MainWindow::setBusy(bool busy)
{
    const bool hasGame = QFileInfo::exists(m_isoPath) && looksLikeDiscImage(m_isoPath);

    // The worker itself is a QProcess, so building is asynchronous. Keep the
    // UI useful while it runs: Graphics/Input/Advanced/logs and compiler fields
    // remain interactive. Only actions that would start a second task or swap
    // the currently compiled disc are locked.
    m_llvmBrowse->setEnabled(true);
    m_backend->setEnabled(true);
    m_profile->setEnabled(true);
    m_toolchain->setEnabled(true);
    m_nativeAbi->setEnabled(m_backend->currentData().toString() == "llvm");
    m_llvmDir->setEnabled(true);
    m_jobs->setEnabled(true);
    m_runArgs->setEnabled(true);

    const bool canPgo = !busy && hasGame && m_backend->currentData().toString() == "llvm";
    m_pgo->setEnabled(canPgo);
    m_inspect->setEnabled(!busy && hasGame);
    m_compile->setEnabled(!busy && hasGame);
    m_buildTools->setEnabled(!busy);
    m_clean->setEnabled(!busy);

    if (m_openAction)
        m_openAction->setEnabled(!busy);
    if (m_playAction)
        m_playAction->setEnabled(!busy && hasGame);
    if (m_stopAction)
        m_stopAction->setEnabled(busy);
    if (m_graphicsAction)
        m_graphicsAction->setEnabled(true);
    if (m_inputAction)
        m_inputAction->setEnabled(true);
    if (m_advancedAction)
        m_advancedAction->setEnabled(true);
    if (m_analyzeAction)
        m_analyzeAction->setEnabled(!busy && hasGame);
    if (m_compileAction)
        m_compileAction->setEnabled(!busy && hasGame);
    if (m_pgoAction)
        m_pgoAction->setEnabled(canPgo);
    if (m_buildToolsAction)
        m_buildToolsAction->setEnabled(!busy);
    if (m_cleanAction)
        m_cleanAction->setEnabled(!busy);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls())
        return;

    for (const QUrl& url : event->mimeData()->urls())
    {
        if (url.isLocalFile() && looksLikeDiscImage(url.toLocalFile()))
        {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    for (const QUrl& url : event->mimeData()->urls())
    {
        if (url.isLocalFile() && looksLikeDiscImage(url.toLocalFile()))
        {
            setIsoPath(url.toLocalFile(), true);
            event->acceptProposedAction();
            return;
        }
    }
}
