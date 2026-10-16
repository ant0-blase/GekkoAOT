#include "MainWindow.hpp"
#include "MemoryCardManager.hpp"
#include "GraphicsConfigDialog.hpp"
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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    // Styling backgrounds alone leaves the Windows/Fusion light palette in
    // unstyled controls and dock titles. Set every shared foreground role too.
    QApplication::setPalette(launcherPalette());
    QApplication::setFont(shellBodyFont());
    setAcceptDrops(true);
    buildUi();
    buildMenusAndToolbar();
    connectUi();
    loadSettings();
    m_library->selected = [this](const QString& path) { if (!m_controller.isRunning()) setIsoPath(path, false); };
    m_library->activated = [this](const QString& path) {
        if(!m_controller.isRunning())setIsoPath(path,true);
        else if(m_runtimeStarted && path!=m_isoPath) {
            m_pendingGame=path;m_pendingGameOnDesktop=true;m_stopAction->trigger();
        } else if(m_runtimeStarted && m_runtimeOnDesktop) {
            if(m_paused)m_pauseAction->trigger();
            m_gameWindow->raise();m_gameWindow->activateWindow();
        }
    };
    m_library->compileRequested = [this](const QStringList& paths) { enqueueCompilations(paths); };
    m_library->changed = [this] { refreshLivingRoom(); };
    m_compilationSidebar->compileRequested = m_library->compileRequested;
    m_compilationSidebar->playRequested = [this](const QString& path){launchCompiledGame(path);};
    m_library->error = [this](const QString& error) { m_log->appendPlainText("[library] " + error); };
    m_library->metadataReady = [this](const QString& path) { m_discordTitles.remove(path); refreshDiscordPresence(); if (QFileInfo(path).canonicalFilePath() == QFileInfo(m_isoPath).canonicalFilePath()) { updateLoaderBanner(); updateGameEnhancementsMenu(); } };
    m_library->importCachedGames();
    if (!m_isoPath.isEmpty()) { m_library->addGame(m_isoPath); m_library->selectGame(m_isoPath); }
    updateGameMetadata();
    m_livingRoom->setGames(m_library->entries());
    m_livingRoom->setCurrentGame(m_isoPath);
    m_livingRoom->setReducedMotion(QSettings("GekkoAOT", "GekkoAOT").value("ui/reducedMotion", false).toBool());
    qApp->installEventFilter(this);

    const QSize available = screen()->availableGeometry().size();
    resize(qMin(1250, available.width() - 30), qMin(800, available.height() - 60));
    setMinimumSize(860, 620);
    statusBar()->hide();statusBar()->setMaximumHeight(0);
    setWindowTitle(QString("GekkoAOT v%1").arg(QString::fromUtf8(GEKKOAOT_VERSION)));
    updateApplicationIcon();
    refreshDiscordPresence();
    shellAnimateButtons(this);
}

MainWindow::~MainWindow()
{
    m_discord.shutdown();
    // Independent top-levels let the TV menu cover the game on compositors
    // that keep transient children above their parent unconditionally.
    delete m_gameWindow;
}

void MainWindow::buildUi()
{
    setStyleSheet(QStringLiteral(R"QSS(
        QMainWindow, QWidget#Root {
            background: #222222;
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
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: #bdbdbd; }
        QPlainTextEdit {
            background: #181818;
            border: 1px solid #444444;
            border-radius: 4px;
            color: #e2e2e2;
            selection-background-color: #565656;
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
    )QSS") + launcherChromeStyleSheet());

    auto* root = new QWidget(this);
    root->setObjectName("Root");
    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_viewFrame = new QWidget(root);
    m_viewFrame->setMinimumHeight(420);
    m_viewStack = new QStackedLayout(m_viewFrame);
    m_viewStack->setContentsMargins(0, 0, 0, 0);
    m_viewStack->setStackingMode(QStackedLayout::StackOne);

    auto* session=new SessionWindow(nullptr);m_gameWindow=session;m_gameWindow->setObjectName("desktopGameWindow");m_gameWindow->resize(1000,650);
    m_gameWindow->setWindowTitle("GekkoAOT");m_gameWindow->setWindowIcon(shellProjectIcon());
    m_gameWindow->setStyleSheet("QMainWindow {background:#242424;} QWidget#gameHostRoot {background:#242424;}");
    auto* gameRoot=new QWidget;gameRoot->setObjectName("gameHostRoot");auto* gameLayout=new QVBoxLayout(gameRoot);gameLayout->setContentsMargins(0,0,0,0);gameLayout->setSpacing(0);
    gameLayout->addWidget(new ShellTitleBar(m_gameWindow));
    m_desktopGameViewport=new NativeRenderWidget(gameRoot);m_desktopGameViewport->setObjectName("desktopGameViewport");
    for(auto attribute:{Qt::WA_NativeWindow,Qt::WA_PaintOnScreen,Qt::WA_NoSystemBackground})m_desktopGameViewport->setAttribute(attribute);
    m_desktopGameViewport->setFocusPolicy(Qt::StrongFocus);m_desktopGameViewport->setMinimumSize(640,360);gameLayout->addWidget(m_desktopGameViewport,1);m_gameWindow->setCentralWidget(gameRoot);
    session->closing=[this]{if(m_runtimeOnDesktop && m_controller.isRunning()){m_stopRequested=true;m_controller.stop();}};


    // v359: the performance HUD is drawn by the renderer into the game image
    // (embedded, standalone and fullscreen windows alike); g_perfHud stays null.

    QString discReader = QDir(QCoreApplication::applicationDirPath()).filePath("gekkoaot-native-disc");
#ifdef Q_OS_WIN
    discReader += ".exe";
#endif
    m_library = new GameLibrary(runtimeStateRoot(), discReader, m_viewFrame);
    m_viewStack->addWidget(m_library);
    m_viewStack->setCurrentWidget(m_library);
    m_livingRoom = new LivingRoomView(m_viewFrame);m_livingRoom->setProperty("shellSceneTransition",true);
    m_viewStack->addWidget(m_livingRoom);
    m_shellGamepad = new ShellGamepad(this);
    m_compileDialog = new QDialog(this);
    m_compileDialog->setObjectName("compilationProgress");
    m_compileDialog->setWindowTitle("Compilation");
    m_compileDialog->setModal(false);
    m_compileDialog->resize(620, 450);
    m_compileDialog->setStyleSheet(settingsStyleSheet() + "QWidget#LoaderOverlay { background: #2b2b2b; } QLabel#LoaderStatus, QLabel#LoaderHint { background: transparent; }");
    auto* compileLayout = new QVBoxLayout(m_compileDialog);
    compileLayout->setContentsMargins(0, 0, 0, 0);
    m_loaderOverlay = new QWidget(m_compileDialog);
    m_loaderOverlay->setObjectName("LoaderOverlay");
    // Do not mark the Qt loader as an opaque paint surface. Its child labels
    // are transparent by default, and rapid status updates can otherwise leave
    // old glyphs in the backing store (the "stacked text" artefact).
    m_loaderOverlay->setAttribute(Qt::WA_StyledBackground, true);
    auto* loaderOuter = new QVBoxLayout(m_loaderOverlay);
    loaderOuter->setContentsMargins(30, 24, 30, 24);
    loaderOuter->addStretch(2);

    m_bannerLabel = new QLabel(m_loaderOverlay);
    m_bannerLabel->setObjectName("GameBanner");
    m_bannerLabel->setAlignment(Qt::AlignCenter);
    m_bannerLabel->setFixedSize(288, 96);
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
    m_loaderStatus->setWordWrap(true);
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

    compileLayout->addWidget(m_loaderOverlay);
    auto* compileButtons = new QDialogButtonBox(QDialogButtonBox::Cancel, m_compileDialog);
    auto* compileLog = compileButtons->addButton("Show log", QDialogButtonBox::HelpRole);
    connect(compileLog, &QPushButton::clicked, this, [this] { setLogVisible(true); });
    connect(compileButtons, &QDialogButtonBox::rejected, this, [this] { m_stopRequested = true; m_controller.stop(); });
    compileLayout->addWidget(compileButtons);
    outer->addWidget(m_viewFrame, 1);

    // Advanced settings live in their own non-modal window. Keeping this out
    // of the main vertical layout prevents the loader, settings and log panes
    // from crushing each other when the window is not tall enough.
    m_advancedDialog = new QDialog(this);
    m_advancedDialog->setWindowTitle("Advanced Settings");
    m_advancedDialog->setModal(false);
    m_advancedDialog->setObjectName("advancedSettings");
    m_advancedDialog->setStyleSheet(settingsStyleSheet());
    m_advancedDialog->setMinimumSize(740, 520);
    m_advancedDialog->resize(860, 620);
    auto* advancedDialogLayout = new QVBoxLayout(m_advancedDialog);
    advancedDialogLayout->setContentsMargins(12, 12, 12, 12);
    advancedDialogLayout->setSpacing(10);

    m_advancedPanel = new QWidget(m_advancedDialog);
    auto* advancedOuter = new QVBoxLayout(m_advancedPanel);
    advancedOuter->setContentsMargins(0, 0, 0, 0);
    advancedOuter->setSpacing(10);

    auto* advancedTabs = new QTabWidget(m_advancedPanel);
    advancedTabs->setObjectName("advancedTabs");
    const auto advancedPage = [&](const QString& title) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(16);
        advancedTabs->addTab(page, title);
        return layout;
    };
    auto* compilationPage = advancedPage("Compilation");
    auto* runtimePage = advancedPage("Runtime");
    auto* maintenancePage = advancedPage("Tools");
    const auto advancedForm = [](QVBoxLayout* page) {
        auto* form = new QFormLayout;
        form->setHorizontalSpacing(24);
        form->setVerticalSpacing(16);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        page->addLayout(form);
        return form;
    };
    auto* settingsForm = advancedForm(compilationPage);
    auto* runtimeForm = advancedForm(runtimePage);
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

    m_nativeOsCheck = new QCheckBox("Enable NativeOS SDK interception", m_advancedPanel);
    m_nativeOsCheck->setToolTip(
        "Compile/run recognized GameCube SDK OS calls through NativeOS. "
        "Disable for the exact guest-AOT SDK path.");
    m_nativeHwCheck = new QCheckBox("Enable NativeHW", m_advancedPanel);
    m_nativeHwCheck->setToolTip(
        "Enabled by default on fresh installations. Enable the full NativeHW "
        "host-service profile: SI/input, DI/NativeVFS, "
        "EXI/memory-card+RTC+SRAM, VI/presentation, AI/audio timing, PI/interrupt "
        "aggregation, MI/host memory, CP/direct Aurora and DSP native task/audio "
        "backends. Guest-visible MMIO/IRQ contracts remain as exact fallbacks.");

    m_runtimeWorkers = new QComboBox(m_advancedPanel);
    m_runtimeWorkers->addItem("Parallel CPU + GX + I/O (recommended)", "parallel");
    m_runtimeWorkers->addItem("Serialized GX / compatibility", "serial");
    m_runtimeWorkers->setToolTip(
        "Parallel keeps guest CPU/NativeOS/MMIO/IRQ ordered on one CPU worker, "
        "while GX and safe I/O services run on separate workers. DSP guest timing "
        "remains CPU-ordered; host audio/render workers remain asynchronous.");

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
    runtimeForm->addRow("NativeOS", m_nativeOsCheck);
    runtimeForm->addRow("NativeHW", m_nativeHwCheck);
    runtimeForm->addRow("Runtime workers", m_runtimeWorkers);
    settingsForm->addRow("LLVM", llvmRow);
    settingsForm->addRow("Parallel jobs", m_jobs);
    runtimeForm->addRow("Extra runner arguments", m_runArgs);
    compilationPage->addStretch();
    runtimePage->addStretch();
    auto* applyHint = mutedLabel("Changes apply to the next launch.", m_advancedPanel);
    runtimePage->addWidget(applyHint);
    auto* advancedButtons = new QGridLayout;
    advancedButtons->setHorizontalSpacing(16);
    advancedButtons->setVerticalSpacing(16);
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
    int toolRow = 0;
    for (const auto& tool : QList<QPair<QPushButton*, QString>>{
        {m_compile, "Build the selected game's native module"},
        {m_pgo, "Optimize from a representative gameplay session"},
        {m_inspect, "Inspect the disc and its executable files"},
        {m_buildTools, "Rebuild the compiler and runtime tools"},
        {m_clean, "Remove the selected game's generated cache"}}) {
        tool.first->setMinimumWidth(170);
        advancedButtons->addWidget(tool.first, toolRow, 0);
        auto* description = mutedLabel(tool.second, m_advancedPanel);
        description->setWordWrap(true);
        advancedButtons->addWidget(description, toolRow++, 1);
    }
    advancedButtons->setColumnStretch(1, 1);
    maintenancePage->addLayout(advancedButtons);
    maintenancePage->addStretch();
    advancedOuter->addWidget(advancedTabs, 1);

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
    maintenancePage->addWidget(revisions);
    auto* advancedClose = new QPushButton("Close", m_advancedPanel);
    connect(advancedClose, &QPushButton::clicked, m_advancedDialog, &QDialog::close);
    debugRow->addWidget(advancedClose);
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
    m_compilationDock = new QDockWidget("Compilations", this);
    m_compilationDock->setObjectName("compilationDock");
    m_compilationDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_compilationSidebar = new CompilationSidebar(&m_compilations, m_compilationDock);
    m_compilationDock->setWidget(m_compilationSidebar);
    addDockWidget(Qt::RightDockWidgetArea, m_compilationDock);
    m_compilationDock->hide();
    m_compilationsAction = new QAction("Compilations",this);
    m_compilationsAction->setIcon(themedIcon(this, "view-list-details", QStyle::SP_FileDialogListView));
    m_compilationsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_B));
    auto* fileMenu = menuBar()->addMenu("&File");
    m_openAction = fileMenu->addAction(themedIcon(this, "document-open", QStyle::SP_DialogOpenButton),
                                       "Open Game…");
    fileMenu->addSeparator();
    fileMenu->addAction("Quit", this, &QWidget::close);

    m_openAction->setShortcut(QKeySequence::Open);

    auto* emuMenu = menuBar()->addMenu("&Session");
    m_playAction = emuMenu->addAction(themedIcon(this, "media-playback-start", QStyle::SP_MediaPlay),
                                      "Play");
    m_stopAction = emuMenu->addAction(themedIcon(this, "media-playback-stop", QStyle::SP_MediaStop),
                                      "Stop");
    m_pauseAction = emuMenu->addAction(themedIcon(this, "media-playback-pause", QStyle::SP_MediaPause), "Pause");
    m_pauseAction->setObjectName("pauseGame");
    m_pauseAction->setCheckable(true);
    m_pauseAction->setEnabled(false);
    m_pauseAction->setShortcut(QKeySequence(Qt::Key_F6));
    emuMenu->addSeparator();
    m_fullscreenAction = emuMenu->addAction(
        themedIcon(this, "view-fullscreen", QStyle::SP_TitleBarMaxButton),
        "Fullscreen Game View");
    m_fullscreenAction->setCheckable(true);
    m_fullscreenAction->setText("Fullscreen game");
    m_tvAction = emuMenu->addAction(themedIcon(this, "video-display", QStyle::SP_ComputerIcon), "TV Mode");
    m_tvAction->setObjectName("tvMode");
    connect(m_tvAction, &QAction::triggered, this, &MainWindow::enterTVMode);
    auto* fullscreenKey = new QShortcut(QKeySequence(Qt::Key_F11), this);
    fullscreenKey->setObjectName("fullscreenShortcut");
    fullscreenKey->setContext(Qt::ApplicationShortcut);
    fullscreenKey->setAutoRepeat(false);
    connect(fullscreenKey, &QShortcut::activated, this, &MainWindow::toggleGameViewFullscreen);
    auto* home = new QShortcut(QKeySequence(Qt::Key_Home), this);
    home->setAutoRepeat(false);
    home->setContext(Qt::ApplicationShortcut);
    connect(home, &QShortcut::activated, this, &MainWindow::toggleHomeMenu);

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
                                           "Compile selected");
    toolsMenu->addAction(m_compilationsAction);
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

    menuBar()->hide();
    m_mainToolbar = addToolBar("Navigation");
    m_mainToolbar->setObjectName("desktopNavigation");
    m_mainToolbar->setMovable(false);
    m_mainToolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_mainToolbar->setIconSize(QSize(19,19));
    m_mainToolbar->setStyleSheet(R"(
        QToolBar { background: #282828; border: 1px solid rgba(255,255,255,42); border-radius: 22px; padding: 10px 14px; spacing: 4px; }
        QToolButton { border-radius: 15px; padding: 10px 14px; color: #dedede; border: 1px solid transparent; }
        QToolButton:hover { background: rgba(255,255,255,14); border-color: #555555; }
        QToolButton:focus { border-color: #eeeeee; }
        QToolButton:disabled { color: #737373; }
        QToolButton#primaryPlay { background: #eeeeee; color: #202020; border: 1px solid #eeeeee; }
        QToolButton#primaryPlay:disabled { background: #383838; color: #777777; border-color: #444444; }
        QToolButton::menu-indicator { image: none; }
    )");
    // Equal outer columns keep transport controls centered in the window,
    // independently of the width of the identity and utility groups.
    auto* content=new QWidget(m_mainToolbar);content->setObjectName("desktopNavContent");
    content->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
    auto* navigation=new QGridLayout(content);navigation->setContentsMargins(0,0,0,0);navigation->setHorizontalSpacing(12);
    navigation->setColumnStretch(0,1);navigation->setColumnStretch(2,1);
    auto* identity=new QWidget(content);auto* identityRow=new QHBoxLayout(identity);identityRow->setContentsMargins(0,0,0,0);identityRow->setSpacing(8);
    auto* logo=new QLabel(identity);logo->setObjectName("desktopProjectLogo");logo->setFixedSize(36,36);logo->setPixmap(shellProjectIcon().pixmap(36,36));logo->setVisible(QSettings("GekkoAOT","GekkoAOT").value("ui/showProjectLogo",true).toBool());identityRow->addWidget(logo);
    auto* brand=new QLabel("GEKKOAOT",identity);brand->setObjectName("desktopBrand");brand->setFont(shellDisplayFont(20));brand->setStyleSheet("color:#eeeeee;padding:0 8px 0 2px;");identityRow->addWidget(brand);
    auto* menuButton=new QToolButton(identity);menuButton->setObjectName("desktopMenu");menuButton->setIcon(shellIcon("menu"));menuButton->setToolTip("Menu · Alt+M");menuButton->setAccessibleName("Menu");identityRow->addWidget(menuButton);
    connect(menuButton,&QToolButton::clicked,this,[this]{showNavigationPanel(false);});
    navigation->addWidget(identity,0,0,Qt::AlignLeft);
    auto* transport=new QWidget(content);transport->setObjectName("desktopTransport");auto* transportRow=new QHBoxLayout(transport);transportRow->setContentsMargins(0,0,0,0);transportRow->setSpacing(4);
    auto tool=[&](QAction* action,QHBoxLayout* row,const QString& name){
        auto* button=new QToolButton(row->parentWidget());button->setObjectName(name);button->setDefaultAction(action);button->setToolButtonStyle(Qt::ToolButtonIconOnly);button->setIconSize({20,20});button->setFixedSize(50,46);button->setAccessibleName(action->text());row->addWidget(button);return button;
    };
    auto* play=tool(m_playAction,transportRow,"primaryPlay");play->setIcon(shellIcon("play",true));
    connect(m_playAction,&QAction::changed,play,[play]{play->setIcon(shellIcon("play",true));});
    tool(m_pauseAction,transportRow,"pauseButton");tool(m_stopAction,transportRow,"stopButton");
    navigation->addWidget(transport,0,1,Qt::AlignCenter);
    auto* utilities=new QWidget(content);auto* utilityRow=new QHBoxLayout(utilities);utilityRow->setContentsMargins(0,0,0,0);utilityRow->setSpacing(4);
    tool(m_compilationsAction,utilityRow,"compilationsButton");tool(m_tvAction,utilityRow,"tvModeButton");
    auto* windowChrome=new ShellTitleBar(this);windowChrome->integrateWindowControls(utilityRow);
    navigation->addWidget(utilities,0,2,Qt::AlignRight);
    m_mainToolbar->addWidget(content);
    m_gameEnhancementsButton=new QToolButton(m_mainToolbar);m_gameEnhancementsButton->setMenu(m_gameEnhancementsMenu);m_gameEnhancementsButton->hide();
    m_openAction->setToolTip("Open a game · Ctrl+O");
    for(auto* action:{m_openAction,m_playAction,m_pauseAction,m_stopAction,m_tvAction,m_compilationsAction})addAction(action);
    removeToolBar(m_mainToolbar);
    m_mainToolbar->setParent(centralWidget());
    auto* shellLayout=qobject_cast<QVBoxLayout*>(centralWidget()->layout());
    shellLayout->setContentsMargins(16,14,16,8);shellLayout->setSpacing(12);shellLayout->insertWidget(0,m_mainToolbar);
    m_mainToolbar->installEventFilter(this);
    shellShadow(m_mainToolbar,16);m_mainToolbar->show();
    for(auto* button:m_mainToolbar->findChildren<QToolButton*>())button->setFocusPolicy(Qt::StrongFocus);

    auto* openMenu=new QShortcut(QKeySequence(Qt::ALT|Qt::Key_M),this);connect(openMenu,&QShortcut::activated,this,[this]{showNavigationPanel(false);});

}

void MainWindow::connectUi()
{
    connect(m_livingRoom, &LivingRoomView::playRequested, this, [this](const QString& path) {
        if (!m_controller.isRunning()) setIsoPath(path, true);
        else if(m_runtimeStarted && path==m_isoPath)dismissLivingRoom(true);
        else if (m_runtimeStarted && path != m_isoPath) {
            m_pendingGame = path;
            m_pendingGameOnDesktop = false;
            m_livingRoom->setBuildStatus("Closing current game…", 0);
            m_stopAction->trigger();
        }
    });
    connect(m_livingRoom, &LivingRoomView::compileRequested, this, [this](const QString& path) { enqueueCompilations({path}); });
    connect(m_livingRoom, &LivingRoomView::resumeRequested, this, [this] { dismissLivingRoom(true); });
    connect(m_livingRoom, &LivingRoomView::stopRequested, this, [this] {
        m_pendingGame.clear(); m_stopAction->trigger();
    });
    connect(m_livingRoom, &LivingRoomView::desktopRequested, this, [this] { setGameViewFullscreen(false); });
    connect(m_livingRoom, &LivingRoomView::backRequested, this, [this] {
        if (m_runtimeStarted) dismissLivingRoom(m_shellPausedGame); else setGameViewFullscreen(false);
    });
    connect(m_livingRoom, &LivingRoomView::openRequested, this, &MainWindow::browseIso);
    connect(m_livingRoom, &LivingRoomView::graphicsRequested, this, &MainWindow::showGraphicsConfig);
    connect(m_livingRoom, &LivingRoomView::inputRequested, this, &MainWindow::showInputConfig);
    connect(m_livingRoom, &LivingRoomView::advancedRequested, this, [this] { setAdvancedVisible(true); });
    connect(m_livingRoom, &LivingRoomView::queueRequested, this, &MainWindow::showCompilations);
    connect(m_livingRoom, &LivingRoomView::appearanceRequested, this, &MainWindow::showAppearanceConfig);
    connect(m_livingRoom, &LivingRoomView::discordRequested, this, &MainWindow::showDiscordConfig);
    connect(m_livingRoom, &LivingRoomView::memoryCardsRequested, this, &MainWindow::showMemoryCards);
    connect(m_compilationsAction, &QAction::triggered, this, &MainWindow::showCompilations);
    connect(m_livingRoom, &LivingRoomView::reducedMotionChanged, this, [](bool value) {
        QSettings("GekkoAOT", "GekkoAOT").setValue("ui/reducedMotion", value);
    });
    // Never enter QDialog::exec from inside SDL polling: Qt deliberately
    // suppresses recursive activation of the currently executing timer.
    connect(m_shellGamepad, &ShellGamepad::homePressed, this, &MainWindow::toggleHomeMenu, Qt::QueuedConnection);
    connect(m_shellGamepad, &ShellGamepad::navigationKey, this, &MainWindow::routeControllerKey, Qt::QueuedConnection);
    connect(m_shellGamepad, &ShellGamepad::controllerChanged, m_livingRoom, &LivingRoomView::setControllerName);
    connect(qApp, &QApplication::focusChanged, this, [this] { updateShellInput(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this] { updateShellInput(); });
    connect(&m_compilations, &CompilationQueue::jobAdded, this, [this](quint64) {
        if (!m_returnToTV && !shellActiveDialog(this)) showCompilations();
    });
    connect(&m_compilations, &CompilationQueue::countsChanged, this, [this] {
        const int count = m_compilations.activeCount() + m_compilations.pendingCount();
        m_compilationsAction->setText(count ? QString("Compilations (%1)").arg(count) : "Compilations");
        m_livingRoom->setQueueCount(count);
        setBusy(m_controller.isRunning());
        QSettings("GekkoAOT", "GekkoAOT").setValue("build/parallelGames", m_compilations.maxParallel());
        refreshDiscordPresence();
    });
    connect(&m_compilations, &CompilationQueue::jobChanged, this, [this] { refreshDiscordPresence(); });
    connect(&m_compilations, &CompilationQueue::foregroundCancelRequested, this, [this] {
        m_stopRequested = true;
    });
    connect(m_llvmBrowse, &QPushButton::clicked, this, &MainWindow::browseLlvmDir);
    connect(m_openAction, &QAction::triggered, this, &MainWindow::browseIso);
    connect(m_playAction, &QAction::triggered, this, [this] { launchAction("run"); });
    connect(m_pauseAction, &QAction::triggered, this, [this](bool paused) {
        if (!m_runtimeStarted) return;
        m_paused = paused;
        QString error;
        if (!writeFrontendConfig(&error)) {
            m_paused = !paused;
            m_pauseAction->setChecked(m_paused);
            m_log->appendPlainText("[gekkoaot] pause: " + error);
            return;
        }
        if (m_playClock.isValid()) {
            if (paused) m_playSeconds += m_playClock.elapsed() / 1000;
            else m_playClock.restart();
        }
        m_pauseAction->setText(paused ? "Resume" : "Pause");
        m_pauseAction->setIcon(themedIcon(this, paused ? "media-playback-start" : "media-playback-pause",
                                        paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
        statusBar()->showMessage(paused ? "Paused — F6 to resume" : "Game resumed", 3000);
        refreshDiscordPresence();
    });
    connect(m_stopAction, &QAction::triggered, this, [this] {
        if (m_activeAction == "pgo" && m_progressStage == "runtime")
        {
            if (m_paused) m_pauseAction->trigger();
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
    connect(&m_controller, &BuildController::presentationEmbedded, this, [this](bool embedded) { m_runtimeEmbedded = embedded; });
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
                if (m_playClock.isValid()) {
                    if (!m_paused) m_playSeconds += m_playClock.elapsed() / 1000;
                    m_library->addPlayTime(m_isoPath, m_playSeconds);
                    m_playClock.invalidate(); m_playSeconds = 0;
                }
                m_activeAction.clear();
                m_runtimeStarted = false;
                m_discordGameStart = 0;
                m_gameWindow->hide();
                m_shellPausedGame = false;
                m_livingRoom->setGameRunning(false);
        updateApplicationIcon();
                m_livingRoom->setBuildStatus(ok || m_stopRequested ? QString{} : "Launch failed · open Compilations for the log");
                m_paused = false;
                m_pauseAction->setChecked(false);
                m_pauseAction->setEnabled(false);
                m_pauseAction->setText("Pause");
                m_pauseAction->setIcon(themedIcon(this, "media-playback-pause", QStyle::SP_MediaPause));
                m_stopRequested = false;
                refreshDiscordPresence();
                const QString selected = m_library->selectedPath();
                if (!selected.isEmpty() && selected != m_isoPath) setIsoPath(selected, false);
                if (!m_pendingGame.isEmpty()) {
                    const QString next = m_pendingGame;
                    const bool desktop=m_pendingGameOnDesktop;
                    m_pendingGame.clear();
                    QTimer::singleShot(0, this, [this, next, desktop] {
                        if(desktop)restoreDesktopLauncher();else openLivingRoom();
                        setIsoPath(next,true);
                    });
                }
                else if (m_returnToTV) QTimer::singleShot(0, this, &MainWindow::openLivingRoom);
                else if (m_hiddenForGame) { m_hiddenForGame = false; show(); }
            });
}

void MainWindow::loadSettings()
{
    QSettings s("GekkoAOT", "GekkoAOT");
    m_isoPath = s.value("iso").toString();
    m_llvmDir->setText(s.value("llvmDir").toString());
    m_runArgs->setText(s.value("runArgs").toString());
    m_jobs->setValue(s.value("jobs", qMax(1, QThread::idealThreadCount())).toInt());
    m_compilations.setMaxParallel(s.value("build/parallelGames", 2).toInt());

    m_graphicsBackend = s.value("graphics/backend", "Vulkan").toString();
    m_resolution = s.value("graphics/resolution", "1920x1080").toString();
    m_aspectMode = s.value("graphics/aspectMode", "4:3").toString();
    // v128b keeps only two modes. Migrate an old explicit widescreen choice to
    // the new live window mode; Auto/original and unknown values become 4:3.
    if (m_aspectMode == "16:9" || m_aspectMode == "16:10" ||
        m_aspectMode == "21:9" || m_aspectMode == "32:9")
        m_aspectMode = "stretch";
    else if (m_aspectMode != "4:3" && m_aspectMode != "stretch")
        m_aspectMode = "4:3";
    // "auto" means native X11 on an X11 session and an XWayland child on a
    // Wayland session. Pure-Wayland sessions without DISPLAY fall back to a
    // separate native Wayland runtime window because Wayland cannot reparent
    // a surface created by another process into a Qt widget.
    m_windowSystem = "auto";
    m_fullscreen = s.value("graphics/fullscreen", false).toBool();
    m_showFps = s.value("graphics/showFps", true).toBool();
    m_hudItems = s.value("graphics/hudItems", GekkoAOT::GX::HudDefaultItems).toUInt();
    // v358: the unlock re-renders geometry. The old image-blend setting
    // ("graphics/frameInterpolation") is deliberately not migrated.
    m_frameInterpolation = s.value("graphics/frameUnlock", false).toBool();
    m_graphicsExtras.unlock_predict = s.value("graphics/frameUnlockPredict", false).toBool() ? 1u : 0u;
    m_fpsLimit = qBound(1, s.value("graphics/fpsLimit", 120).toInt(), 1000);
    m_msaaSamples = s.value("graphics/msaaSamples", 1).toInt() == 4 ? 4 : 1;
    const int anisotropy = s.value("graphics/textureAnisotropy", 16).toInt();
    m_textureAnisotropy = (anisotropy == 1 || anisotropy == 2 || anisotropy == 4 ||
                           anisotropy == 8 || anisotropy == 16) ? anisotropy : 16;
    m_graphicsExtras.render_percent = qBound(25, s.value("graphics/renderPercent", 100).toInt(), 400);
    m_graphicsExtras.fxaa = s.value("graphics/fxaa", false).toBool();
    m_graphicsExtras.brightness = qBound(0.0, s.value("graphics/brightness", double(m_graphicsExtras.brightness)).toDouble(), 2.0);
    m_graphicsExtras.contrast = qBound(0.0, s.value("graphics/contrast", double(m_graphicsExtras.contrast)).toDouble(), 2.0);
    m_graphicsExtras.saturation = qBound(0.0, s.value("graphics/saturation", double(m_graphicsExtras.saturation)).toDouble(), 2.0);
    m_graphicsExtras.gamma = qBound(0.5, s.value("graphics/gamma", double(m_graphicsExtras.gamma)).toDouble(), 2.5);
    m_graphicsExtras.exposure = qBound(-3.0, s.value("graphics/exposure", double(m_graphicsExtras.exposure)).toDouble(), 3.0);
    m_graphicsExtras.temperature = qBound(-1.0, s.value("graphics/temperature", double(m_graphicsExtras.temperature)).toDouble(), 1.0);
    m_graphicsExtras.vibrance = qBound(0.0, s.value("graphics/vibrance", double(m_graphicsExtras.vibrance)).toDouble(), 1.0);
    m_graphicsExtras.tint = qBound(-1.0, s.value("graphics/tint", double(m_graphicsExtras.tint)).toDouble(), 1.0);
    m_postProcessing = s.value("graphics/postProcessing", true).toBool();
    m_postPreset = s.value("graphics/postPreset", "balanced").toString();
    if (m_postPreset != "balanced" && m_postPreset != "cinematic" &&
        m_postPreset != "crisp" && m_postPreset != "neutral" && m_postPreset != "custom")
        m_postPreset = "balanced";
    m_postSharpen = qBound(0.0, s.value("graphics/postSharpen", 0.28).toDouble(), 1.0);
    m_postBloom = qBound(0.0, s.value("graphics/postBloom", 0.10).toDouble(), 1.0);
    m_postVignette = qBound(0.0, s.value("graphics/postVignette", 0.06).toDouble(), 1.0);
    m_postGrain = qBound(0.0, s.value("graphics/postGrain", 0.0).toDouble(), 0.25);
    m_postTone = qBound(0.0, s.value("graphics/postTone", 0.15).toDouble(), 1.0);
    m_postLocalContrast = qBound(0.0, s.value("graphics/postLocalContrast", 0.08).toDouble(), 1.0);
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
    restoreCombo(m_runtimeWorkers, "advanced/runtimeWorkers", "parallel");
    // NativeOS is part of the validated low-CPU configuration (idle/spin
    // parking, SDK services); a fresh install starts with it enabled.
    m_nativeOsCheck->setChecked(s.value("advanced/nativeOS", true).toBool());
    m_nativeHwCheck->setChecked(s.value("advanced/nativeHW", true).toBool());
    m_nativeAbi->setEnabled(m_backend->currentData().toString() == "llvm");
    m_discord.configure(s.value("discord/enabled", true).toBool(),
        s.value("discord/applicationId", DiscordPresence::defaultApplicationId()).toString());
}

void MainWindow::saveSettings() const
{
    QSettings s("GekkoAOT", "GekkoAOT");
    s.setValue("iso", m_isoPath);
    s.setValue("llvmDir", m_llvmDir->text());
    s.setValue("runArgs", m_runArgs->text());
    s.setValue("jobs", m_jobs->value());
    s.setValue("build/parallelGames", m_compilations.maxParallel());
    s.setValue("backend", m_backend->currentData());
    s.setValue("profile", m_profile->currentData());
    s.setValue("toolchain", m_toolchain->currentData());
    s.setValue("nativeAbi", m_nativeAbi->currentData());
    s.setValue("advanced/nativeOS", m_nativeOsCheck->isChecked());
    s.setValue("advanced/nativeHW", m_nativeHwCheck->isChecked());
    s.setValue("advanced/runtimeWorkers", m_runtimeWorkers->currentData());

    s.setValue("graphics/backend", m_graphicsBackend);
    s.setValue("graphics/resolution", m_resolution);
    s.setValue("graphics/aspectMode", m_aspectMode);
    s.setValue("graphics/fullscreen", m_fullscreen);
    s.setValue("graphics/showFps", m_showFps);
    s.setValue("graphics/frameUnlock", m_frameInterpolation);
    s.setValue("graphics/frameUnlockPredict", m_graphicsExtras.unlock_predict != 0);
    s.setValue("graphics/hudItems", m_hudItems);
    s.setValue("graphics/fpsLimit", m_fpsLimit);
    s.setValue("graphics/msaaSamples", m_msaaSamples);
    s.setValue("graphics/textureAnisotropy", m_textureAnisotropy);
    s.setValue("graphics/renderPercent", int(m_graphicsExtras.render_percent));
    s.setValue("graphics/fxaa", bool(m_graphicsExtras.fxaa));
    s.setValue("graphics/brightness", m_graphicsExtras.brightness);
    s.setValue("graphics/contrast", m_graphicsExtras.contrast);
    s.setValue("graphics/saturation", m_graphicsExtras.saturation);
    s.setValue("graphics/gamma", m_graphicsExtras.gamma);
    s.setValue("graphics/exposure", m_graphicsExtras.exposure);
    s.setValue("graphics/temperature", m_graphicsExtras.temperature);
    s.setValue("graphics/vibrance", m_graphicsExtras.vibrance);
    s.setValue("graphics/tint", m_graphicsExtras.tint);
    s.setValue("graphics/postProcessing", m_postProcessing);
    s.setValue("graphics/postPreset", m_postPreset);
    s.setValue("graphics/postSharpen", m_postSharpen);
    s.setValue("graphics/postBloom", m_postBloom);
    s.setValue("graphics/postVignette", m_postVignette);
    s.setValue("graphics/postGrain", m_postGrain);
    s.setValue("graphics/postTone", m_postTone);
    s.setValue("graphics/postLocalContrast", m_postLocalContrast);
    s.setValue("input/controllers", m_controllers);
}

void MainWindow::browseIso()
{
    if (m_controller.isRunning()) return;
    QFileDialog browser(this,"Choose a GameCube/Wii game",m_isoPath.isEmpty()?QString{}:QFileInfo(m_isoPath).absolutePath());
    browser.setOption(QFileDialog::DontUseNativeDialog);
    browser.setFileMode(QFileDialog::ExistingFile);
    browser.setNameFilters({"Game sources (*.iso *.gcm *.wbfs *.rvz *.wia *.gcz *.dol)","All files (*)"});
    presentShellDialog(browser,"Open game",m_shellVisible);
    if(execShellDialog(browser)==QDialog::Accepted && !browser.selectedFiles().isEmpty())setIsoPath(browser.selectedFiles().first(),true);
}

void MainWindow::browseLlvmDir()
{
    const QString path = shellChooseFiles(this,"Select LLVM CMake directory",{},false,true).value(0);
    if (!path.isEmpty())
        m_llvmDir->setText(path);
}

void MainWindow::setIsoPath(const QString& path, bool autoPlay)
{
    m_isoPath = QFileInfo(path).absoluteFilePath();
    m_library->addGame(m_isoPath);
    m_library->selectGame(m_isoPath);
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
    if (!path.isEmpty()) {
        const QString title = readDiscMetadata(discMetadataCachePath(runtimeStateRoot(), path))["title"].toString();
        if (!title.isEmpty()) return title;
    }
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
    if (!path.isEmpty()) {
        const QString id = readDiscMetadata(discMetadataCachePath(runtimeStateRoot(), path))["gameId"].toString();
        if (!id.isEmpty()) return id;
    }
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
    QFile libraryBanner(discMetadataCachePath(runtimeStateRoot(), path) + "/meta/opening.bnr");
    if (libraryBanner.open(QIODevice::ReadOnly)) {
        const auto image = decodeGameCubeBanner(libraryBanner.readAll());
        if (!image.isNull()) return image;
    }
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
           "[Emulation]\n"
        << "paused=" << (m_paused ? "true" : "false") << "\n"
        << "[Video]\n"
        << "backend=" << m_graphicsBackend << "\n"
        << "aspect_ratio=" << m_aspectMode << "\n"
        << "fullscreen=" << (m_fullscreen ? "true" : "false") << "\n"
        << "show_fps_in_title=" << (m_showFps ? "true" : "false") << "\n"
        << "fps_limit=" << m_fpsLimit << "\n"
        << "frame_interpolation=" << (m_frameInterpolation ? 1 : 0) << "\n"
        << "frame_unlock_predict=" << m_graphicsExtras.unlock_predict << "\n"
        << "hud=" << (m_showFps ? 1 : 0) << "\n";
    for (const auto& item : GekkoAOT::GX::HudItemKeys)
        out << item.key << "=" << ((m_hudItems & item.bit) ? 1 : 0) << "\n";
    out
        << "render_percent=" << m_graphicsExtras.render_percent << "\n"
        << "render_width=" << (screen()->size() * screen()->devicePixelRatio()).width() << "\n"
        << "render_height=" << (screen()->size() * screen()->devicePixelRatio()).height() << "\n"
        << "fxaa=" << m_graphicsExtras.fxaa << "\n"
        << "post_processing=" << (m_postProcessing ? 1 : 0) << "\n"
        << "post_sharpen=" << m_postSharpen << "\n"
        << "post_bloom=" << m_postBloom << "\n"
        << "post_vignette=" << m_postVignette << "\n"
        << "post_grain=" << m_postGrain << "\n"
        << "post_tone=" << m_postTone << "\n"
        << "post_local_contrast=" << m_postLocalContrast << "\n"
        << "post_cinematic=" << (m_postPreset == "cinematic" ? .20 : 0.) << "\n"
        << "post_brightness=" << m_graphicsExtras.brightness << "\n"
        << "post_contrast=" << m_graphicsExtras.contrast << "\n"
        << "post_saturation=" << m_graphicsExtras.saturation << "\n"
        << "post_gamma=" << m_graphicsExtras.gamma << "\n"
        << "post_exposure=" << m_graphicsExtras.exposure << "\n"
        << "post_temperature=" << m_graphicsExtras.temperature << "\n"
        << "post_vibrance=" << m_graphicsExtras.vibrance << "\n"
        << "post_tint=" << m_graphicsExtras.tint << "\n"
        << "msaa_samples=" << m_msaaSamples << "\n"
        << "texture_anisotropy=" << m_textureAnisotropy << "\n"
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

QMap<QString, QString> MainWindow::gameEnhancementEnvironment(const QString& path) const
{
    QMap<QString, QString> environment;
    const QString discId = discIdForPath(path.isEmpty() ? m_isoPath : path);
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
    const QScreen* current = m_gameWindow && m_gameWindow->isVisible() ? m_gameWindow->screen() : screen();
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
    GraphicsOptions options;
    options.image=m_graphicsExtras;
    options.image.msaa=m_msaaSamples; options.image.anisotropy=m_textureAnisotropy;
    options.image.post=m_postProcessing; options.image.interpolation=m_frameInterpolation;
    options.image.sharpen=m_postSharpen; options.image.bloom=m_postBloom;
    options.image.vignette=m_postVignette; options.image.grain=m_postGrain;
    options.image.tone=m_postTone; options.image.local_contrast=m_postLocalContrast;
    options.image.cinematic=m_postPreset=="cinematic"?.20f:0.f;
    options.backend=m_graphicsBackend;
    options.aspect=m_aspectMode; options.preset=m_postPreset;
    options.fps=m_fpsLimit; options.fullscreen=m_runtimeStarted?m_gameWindow->isFullScreen():m_fullscreen; options.hud=m_showFps;
    options.hudItems=m_hudItems;
    options.outputExtent = screen()->size() * screen()->devicePixelRatio();
    const auto preview=[this,lastFullscreen=options.fullscreen](const GraphicsOptions& next) mutable {
        const bool fullscreenChanged=next.fullscreen!=lastFullscreen;
        lastFullscreen=next.fullscreen;
        m_graphicsExtras=next.image;
        m_graphicsBackend=next.backend;
        m_aspectMode=next.aspect; m_postPreset=next.preset;
        m_fpsLimit=next.fps; if(fullscreenChanged)m_fullscreen=next.fullscreen; m_showFps=next.hud;
        m_hudItems=next.hudItems;
        if (fullscreenChanged && m_runtimeStarted && m_runtimeEmbedded) {
            // Colour/resolution previews never change window ownership/state.
            // Only an explicit Fullscreen checkbox edit may do so.
            auto* target=m_gameWindow;
            if(m_fullscreen)target->showFullScreen();else target->showNormal();
        }
        m_msaaSamples=next.image.msaa; m_textureAnisotropy=next.image.anisotropy;
        m_postProcessing=next.image.post; m_frameInterpolation=next.image.interpolation;
        m_postSharpen=next.image.sharpen; m_postBloom=next.image.bloom;
        m_postVignette=next.image.vignette; m_postGrain=next.image.grain;
        m_postTone=next.image.tone; m_postLocalContrast=next.image.local_contrast;
        if(g_perfHud) g_perfHud->setVisible(m_showFps && m_runtimeStarted && m_runtimeEmbedded);
        QString error;
        if(!writeFrontendConfig(&error)) m_log->appendPlainText("[gekkoaot] graphics: "+error);
    };
    if(editGraphicsOptions(this,options,m_runtimeStarted,preview)==QDialog::Accepted) {
        saveSettings();
        statusBar()->showMessage("Graphics settings saved",2500);
    }
}

void MainWindow::showInputConfig()
{
    const QString profileFile = QDir(frontendUserDir()).filePath("input.ini");
    auto profiles = loadInputProfiles(profileFile);
    if (editInputProfiles(this, profiles, detectHostInputDevices()) != QDialog::Accepted)
        return;

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

BuildRequest MainWindow::requestFor(const QString& action, const QString& path) const
{
    const QString source = path.isEmpty() ? m_isoPath : path;
    BuildRequest request;
    request.action = action;
    request.isoPath = source;
    request.gameSlug = slugForPath(source);
    request.backend = m_backend->currentData().toString();
    request.graphicsBackend = m_graphicsBackend;
    request.graphics = m_graphicsExtras;
    const QSize renderReference = screen()->size() * screen()->devicePixelRatio();
    request.graphics.render_width = renderReference.width();
    request.graphics.render_height = renderReference.height();
    request.profile = m_profile->currentData().toString();
    request.toolchain = m_toolchain->currentData().toString();
    request.llvmDir = m_llvmDir->text().trimmed();
    request.nativeAbi = m_nativeAbi->currentData().toString();
    request.nativeOs = m_nativeOsCheck->isChecked();
    request.nativeHw = m_nativeHwCheck->isChecked();
    request.runtimeWorkers = m_runtimeWorkers->currentData().toString();
    request.jobs = m_jobs->value();
    request.runArgs = m_runArgs->text().trimmed();
    request.userDir = QDir(runtimeStateRoot()).filePath("user/" + request.gameSlug);
    request.aspectMode = m_aspectMode;
    request.resolution = m_resolution;
    request.fullscreen = m_fullscreen || m_returnToTV;
    request.fpsLimit = m_fpsLimit;
    request.frameInterpolation = m_frameInterpolation;
    request.msaaSamples = m_msaaSamples;
    request.textureAnisotropy = m_textureAnisotropy;
    request.postProcessing = m_postProcessing;
    request.postPreset = m_postPreset;
    request.postSharpen = m_postSharpen;
    request.postBloom = m_postBloom;
    request.postVignette = m_postVignette;
    request.postGrain = m_postGrain;
    request.postTone = m_postTone;
    request.postLocalContrast = m_postLocalContrast;
    request.uiScale = QString::number(effectiveDisplayScale(), 'f', 3);
    request.inputConfig = QDir(request.userDir).filePath("input.ini");
    request.persistentShell = true;
    // Keep one render surface alive across Desktop, TV, Home and fullscreen.
    QWidget* renderTarget=m_desktopGameViewport;
    request.gameEnvironment = gameEnhancementEnvironment(source);
    const QString iconPath=QDir(request.userDir).filePath("window-icon.bmp");
    QDir().mkpath(request.userDir);
    QImage banner=decodeGameCubeBanner([&]{QFile file(discMetadataCachePath(runtimeStateRoot(),source)+"/meta/opening.bnr");return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};}());
    QSettings appearance("GekkoAOT","GekkoAOT");
    const auto icon=appearance.value("ui/gameAppIcon",true).toBool()?shellGameIcon(banner):shellProjectIcon();
    if(icon.pixmap(128,128).toImage().convertToFormat(QImage::Format_RGB32).save(iconPath,"BMP"))request.gameEnvironment.insert("GEKKOAOT_WINDOW_ICON",iconPath);


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
        request.embedWindow = QString::number(static_cast<qulonglong>(renderTarget->winId()));
    }
    else if (qEnvironmentVariable("XDG_SESSION_TYPE").compare("wayland", Qt::CaseInsensitive) == 0)
    {
        request.windowSystem = "wayland";
        request.embedWindow.clear();
    }
    else
    {
#ifdef Q_OS_WIN
        request.windowSystem = "windows";
        request.embedWindow = QString::number(static_cast<qulonglong>(renderTarget->winId()));
#else
        request.windowSystem = "auto";
        request.embedWindow.clear();
#endif
    }
    if (request.persistentShell && !request.embedWindow.isEmpty()) request.fullscreen = false;
    return request;
}

void MainWindow::launchAction(const QString& action)
{
    saveSettings();
    if (action == "compile") {
        auto paths = m_library->selectedPaths();
        if (paths.isEmpty() && !m_isoPath.isEmpty()) paths << m_isoPath;
        enqueueCompilations(paths);
        return;
    }
    if (m_controller.isRunning()) return;
    if ((action == "clean" || action == "tools") &&
        (m_compilations.activeCount() || m_compilations.pendingCount())) return;

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

    if(action=="run"||action=="pgo"){m_runtimeOnDesktop=!m_returnToTV;m_gameWindow->setWindowTitle(gameTitleForPath(m_isoPath));}
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

    const auto request = requestFor(action);
    if (action == "run" || action == "pgo")
        m_compilations.trackForeground(&m_controller, request, gameTitle,
            m_library->metadata(m_isoPath)["gameId"].toString());
    m_controller.run(request);
}

void MainWindow::enqueueCompilations(const QStringList& paths)
{
    saveSettings();
    if (m_controller.isRunning() && (m_activeAction == "clean" || m_activeAction == "tools")) {
        statusBar()->showMessage("Wait for engine maintenance to finish", 4000);
        return;
    }
    int count = 0;
    for (const auto& path : paths) {
        if (!QFileInfo(path).isFile() || !looksLikeDiscImage(path)) continue;
        m_library->addGame(path);
        const auto metadata = m_library->metadata(path);
        m_compilations.enqueue(requestFor("compile", path),
            metadata["title"].toString(gameTitleForPath(path)), metadata["gameId"].toString());
        ++count;
    }
    if (count) {
        if (!m_returnToTV && !shellActiveDialog(this)) showCompilations();
        statusBar()->showMessage(QString("%1 game(s) added to compilations").arg(count), 4000);
    }
}

void MainWindow::setAdvancedVisible(bool visible)
{
    if (!m_advancedDialog)
        return;
    if (visible)
    {
        presentShellDialog(*m_advancedDialog,"Advanced",m_shellVisible);
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
    if(m_runtimeStarted) {
        // A fullscreen desktop launcher must always have a way back too.
        if(m_runtimeOnDesktop && !m_shellVisible && isActiveWindow() && isFullScreen()) {restoreDesktopLauncher();return;}
        setGameViewFullscreen(m_shellVisible?false:!m_gameWindow->isFullScreen());
    } else setGameViewFullscreen(!isFullScreen());
}

void MainWindow::enterTVMode()
{
    if(m_runtimeStarted && m_runtimeEmbedded) {
        m_runtimeOnDesktop=false;
        m_gameWindow->showFullScreen();
    }
    openLivingRoom();
}

void MainWindow::openLivingRoom()
{
    m_returnToTV = true;
    centralWidget()->layout()->setContentsMargins(0,0,0,0);centralWidget()->layout()->setSpacing(0);
    if (!m_shellVisible) {
        m_restoreDockAfterTV = m_compilationDock->isVisible();
        m_shellPausedGame = false;
        if (m_runtimeStarted && !m_paused) {
            m_pauseAction->trigger();
            m_shellPausedGame = m_paused;
        }
    }
    m_shellVisible = true;
    setProperty("tvMode",true);
    m_hiddenForGame = false;
    m_livingRoom->setGameRunning(m_runtimeStarted, m_isoPath);
    if (m_runtimeStarted) m_livingRoom->setCurrentGame(m_isoPath);
    m_viewStack->setCurrentWidget(m_livingRoom);
    m_compileDialog->hide();
    m_advancedDialog->hide();
    m_compilationDock->hide();
    menuBar()->hide(); m_mainToolbar->hide(); statusBar()->hide();
    if (!isFullScreen()) {
        m_wasMaximizedBeforeGameFullscreen = isMaximized();
        setWindowState(windowState() | Qt::WindowFullScreen);
        showFullScreen();
    } else show();
    raise(); activateWindow();
    m_livingRoom->setFocus(Qt::ShortcutFocusReason);shellRevealSnapshot(m_livingRoom);
    m_gameViewFullscreen = true;
    m_fullscreenAction->setChecked(true);
    QTimer::singleShot(0, this, [this] { updateShellInput(); });
    refreshDiscordPresence();
}

void MainWindow::toggleHomeMenu()
{
    if(auto* panel=shellActiveDialog(this)){panel->reject();return;}
    if (m_shellVisible && m_runtimeStarted) dismissLivingRoom(m_shellPausedGame);
    else if (m_shellVisible) m_livingRoom->openSettings();
    else openLivingRoom();
}

void MainWindow::dismissLivingRoom(bool resume)
{
    if (resume && m_runtimeStarted && m_paused) m_pauseAction->trigger();
    // A failed pause/resume write leaves the menu visible instead of hiding a
    // game that the host could not resume.
    if (resume && m_runtimeStarted && m_paused) return;
    m_shellPausedGame = false;
    m_shellVisible = false;
    if(m_runtimeStarted && m_runtimeEmbedded) {
        if(m_runtimeOnDesktop) {
            restoreDesktopLauncher();
            m_viewStack->setCurrentWidget(m_library);
        }
        m_gameWindow->show();m_gameWindow->raise();m_gameWindow->activateWindow();m_desktopGameViewport->setFocus();
    } else {
        m_viewStack->setCurrentWidget(m_library);
        if(m_runtimeStarted && m_returnToTV) {m_hiddenForGame=true;hide();}
    }
    updateShellInput();
}

void MainWindow::restoreDesktopLauncher()
{
    m_returnToTV = false;
    m_shellVisible = false;
    m_hiddenForGame = false;
    setProperty("tvMode", false);
    centralWidget()->layout()->setContentsMargins(16,14,16,8);
    centralWidget()->layout()->setSpacing(12);
    if (m_wasMaximizedBeforeGameFullscreen) showMaximized(); else showNormal();
    menuBar()->hide();
    m_mainToolbar->show();
    statusBar()->hide();
    refreshDiscordPresence();
}

void MainWindow::setGameViewFullscreen(bool fullscreen)
{
    if(m_runtimeStarted && m_runtimeEmbedded) {
        if(!fullscreen && (!m_runtimeOnDesktop || m_shellVisible)) {
            // Leaving TV restores the library and detaches presentation only;
            // SDL/Vulkan keep their original native window and running game.
            if(m_shellPausedGame && m_paused)m_pauseAction->trigger();
            m_shellPausedGame=false;m_runtimeOnDesktop=true;
            restoreDesktopLauncher();m_viewStack->setCurrentWidget(m_library);
        }
        if(fullscreen)m_gameWindow->showFullScreen();else m_gameWindow->showNormal();
        m_gameWindow->raise();m_gameWindow->activateWindow();m_desktopGameViewport->setFocus();
        return;
    }
    if(fullscreen) {
        if(!isFullScreen())m_wasMaximizedBeforeGameFullscreen=isMaximized();
        showFullScreen();
    } else {
        m_viewStack->setCurrentWidget(m_library);restoreDesktopLauncher();
    }
    const QSignalBlocker blocker(m_fullscreenAction);
    m_fullscreenAction->setChecked(fullscreen);updateShellInput();
}

void MainWindow::refreshLivingRoom()
{
    if (m_tvRefreshPending) return;
    m_tvRefreshPending = true;
    QTimer::singleShot(100, this, [this] {
        m_tvRefreshPending = false;
        m_livingRoom->setGames(m_library->entries());
    });
}

void MainWindow::updateShellInput()
{
    if (!m_shellGamepad) return;
    QWidget* active=QApplication::activeWindow();
    const bool launcherActive=active==this;
    m_shellGamepad->setNavigationEnabled(launcherActive &&
        (shellActiveDialog(this) || m_shellVisible || m_viewStack->currentWidget()==m_library));
}

void MainWindow::routeControllerKey(int key)
{
    auto owned=[this](QObject* object){for(auto* p=object;p;p=p->parent())if(p==this)return true;return false;};
    if(auto* popup=QApplication::activePopupWidget();popup && owned(popup)) {
        auto* focus=QApplication::focusWidget();
        QWidget* target=focus && (focus==popup || popup->isAncestorOf(focus))?focus:popup;
        QKeyEvent press(QEvent::KeyPress,key,Qt::NoModifier),release(QEvent::KeyRelease,key,Qt::NoModifier);
        QApplication::sendEvent(target,&press);QApplication::sendEvent(target,&release);return;
    }
    if(auto* panel=shellActiveDialog(this)) {
        if(key==Qt::Key_Escape){panel->reject();return;}
        // Optional shortcuts remain conveniences; every tab is also a visible
        // button reachable through normal directional navigation.
        if(key==Qt::Key_PageUp || key==Qt::Key_PageDown) {
            if(auto* tabs=panel->findChild<QTabWidget*>())tabs->setCurrentIndex((tabs->currentIndex()+(key==Qt::Key_PageDown?1:tabs->count()-1))%tabs->count());
            return;
        }
        shellNavigate(panel,key);return;
    }
    if(!isActiveWindow())return;
    if(m_shellVisible) {
        if((key==Qt::Key_Return || key==Qt::Key_Enter))if(auto* edit=qobject_cast<QLineEdit*>(QApplication::focusWidget())){showShellKeyboard(edit);return;}
        m_livingRoom->navigate(key);return;
    }
    if(m_viewStack->currentWidget()!=m_library)return;
    auto* search=m_library->findChild<QLineEdit*>("librarySearch");
    if(key==Qt::Key_Escape){if(isFullScreen())setGameViewFullscreen(false);else shellFocusControl(search);return;}
    if(key==Qt::Key_PageUp){shellFocusControl(findChild<QToolButton*>("desktopMenu"));return;}
    if(key==Qt::Key_PageDown){m_library->focusGames();return;}
    if(m_library->navigateGames(key))return;
    auto* focus=QApplication::focusWidget();
    if(!focus || focus==this){shellFocusControl(search);return;}
    auto* header=m_library->findChild<QWidget*>("libraryControls");
    auto* import=m_library->findChild<QPushButton*>("libraryImportFolder");
    if(focus==import){if(key==Qt::Key_Up)shellFocusControl(findChild<QToolButton*>("desktopMenu"));else shellFocusControl(search);return;}
    if(focus==search || header->isAncestorOf(focus)) {
        if(key==Qt::Key_Right){shellFocusControl(import);return;}
        if(key==Qt::Key_Up){shellFocusControl(findChild<QToolButton*>("desktopMenu"));return;}
        if(key==Qt::Key_Down){m_library->focusGames(true);return;}
        shellNavigate(header,key);return;
    }
    if(m_mainToolbar->isAncestorOf(focus)) {
        if(key==Qt::Key_Down){shellFocusControl(search);return;}
        shellNavigate(m_mainToolbar,key);return;
    }
    shellNavigate(m_library,key);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    if(event->type()==QEvent::KeyPress && shellActiveDialog(this)) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Tab || key->key()==Qt::Key_Backtab){routeControllerKey(key->key());return true;}
    }

    if(watched==m_mainToolbar && event->type()==QEvent::Resize) {
        const bool compact=m_mainToolbar->width()<740;
        if(auto* brand=findChild<QLabel*>("desktopBrand"))brand->setVisible(!compact);
    }

    if (m_shellVisible && event->type() == QEvent::KeyPress) {
        auto* target = qobject_cast<QWidget*>(watched);
        auto* key = static_cast<QKeyEvent*>(event);
        if (target && target->window() == this && (target == m_livingRoom || m_livingRoom->isAncestorOf(target))) {
            const int code = key->key();
            if (!(qobject_cast<QLineEdit*>(target) && (code == Qt::Key_Left || code == Qt::Key_Right)) &&
                (code == Qt::Key_Left || code == Qt::Key_Right || code == Qt::Key_Up || code == Qt::Key_Down ||
                 code == Qt::Key_Return || code == Qt::Key_Enter || code == Qt::Key_Escape)) {
                m_livingRoom->navigate(code); return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (!event) return;
    if (event->type() == QEvent::ActivationChange) { updateShellInput(); return; }
    if (event->type() != QEvent::WindowStateChange) return;
    m_gameViewFullscreen = isFullScreen();
    if (m_fullscreenAction) {
        const QSignalBlocker blocker(m_fullscreenAction);
        m_fullscreenAction->setChecked(m_gameViewFullscreen);
    }
    // Window-manager state changes can arrive after a requested transition.
    // They must not replace the logical TV view or discard an open overlay.
    if (!m_gameViewFullscreen && !m_shellVisible) {
        menuBar()->hide(); if (m_mainToolbar) m_mainToolbar->show(); statusBar()->hide();
        updateShellInput();
    }

}

void MainWindow::showLoader(const QString& title, const QString& status, int progress)
{
    // Runtime output can still contain late build/progress markers. Once a
    // normal run has entered the game view, never cover it with the loader.
    if (m_runtimeStarted && m_activeAction == "run")
        return;
    progress = qBound(0, progress, 100);
    if (!m_activeAction.isEmpty() && progress < m_lastProgress)
        progress = m_lastProgress;
    m_lastProgress = progress;

    m_loaderTitle->setText(title.isEmpty() ? QStringLiteral("GekkoAOT") : title);
    // Clear first so a shorter status cannot retain pixels from the previous
    // one on some XWayland/Qt backing-store combinations.
    m_loaderStatus->clear();
    m_loaderStatus->setText(buildProgressLabel(status));
    m_loaderHint->setText("Build runs in the background · Advanced → Show Log");
    m_progress->setRange(0, 100);
    m_progress->setValue(progress);
    m_progress->setFormat(QString::number(progress) + "%");
    if (m_shellVisible) {
        m_livingRoom->setBuildStatus(status, progress);
        return;
    }
    m_compileDialog->hide();
    if(!m_controller.isRunning() && !shellActiveDialog(this))showCompilations();

}

void MainWindow::showGameViewport()
{
    m_progress->setValue(100);
    m_compileDialog->hide();
    if(auto* queue=findChild<QDialog*>("compilationQueuePanel"))queue->close();
    m_loaderOverlay->hide();
    if(m_runtimeEmbedded) {
        const bool reveal=!m_runtimeStarted && QStringList{"xcb","windows","cocoa"}.contains(QGuiApplication::platformName()) && !QSettings("GekkoAOT","GekkoAOT").value("ui/reducedMotion",false).toBool();
        if(reveal)m_gameWindow->setWindowOpacity(.08);
        m_viewStack->setCurrentWidget(m_returnToTV?static_cast<QWidget*>(m_livingRoom):static_cast<QWidget*>(m_library));
        if(m_returnToTV || m_fullscreen)m_gameWindow->showFullScreen();else m_gameWindow->showNormal();
        m_gameWindow->raise();m_gameWindow->activateWindow();m_desktopGameViewport->setFocus();
        if(reveal){auto* motion=new QPropertyAnimation(m_gameWindow,"windowOpacity",m_gameWindow);motion->setObjectName("gameLaunchMotion");motion->setDuration(320);motion->setStartValue(.08);motion->setEndValue(1.0);motion->setEasingCurve(QEasingCurve::OutCubic);motion->start(QAbstractAnimation::DeleteWhenStopped);}
    } else m_viewStack->setCurrentWidget(m_library);
    if (!m_runtimeStarted) {
        m_discordGameStart = QDateTime::currentSecsSinceEpoch();
        m_playSeconds = 0; m_playClock.start();
        m_library->recordLaunch(m_isoPath);
    }
    m_runtimeStarted = true;
    refreshDiscordPresence();
    updateApplicationIcon();
    updateShellInput();
    m_livingRoom->setGameRunning(true, m_isoPath);
    m_livingRoom->setBuildStatus({});
    m_pauseAction->setEnabled(true);
    if (m_returnToTV) dismissLivingRoom(false);
    if (g_perfHud)
    {
        g_perfHud->setVisible(m_showFps && m_runtimeEmbedded);
        g_perfHud->raise();
    }
}

void MainWindow::showIdleViewport(const QString& message)
{
    if (g_perfHud)
        g_perfHud->hide();
    updateApplicationIcon();
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
    m_compileDialog->hide();
    m_viewStack->setCurrentWidget(m_shellVisible ? static_cast<QWidget*>(m_livingRoom) : static_cast<QWidget*>(m_library));
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
        g_perfHud->setVisible(m_showFps && m_runtimeStarted && m_runtimeEmbedded);
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
    m_nativeOsCheck->setEnabled(true);
    m_nativeHwCheck->setEnabled(true);
    m_runtimeWorkers->setEnabled(true);
    m_llvmDir->setEnabled(true);
    m_jobs->setEnabled(true);
    m_runArgs->setEnabled(true);

    const bool canPgo = !busy && hasGame && m_backend->currentData().toString() == "llvm";
    m_pgo->setEnabled(canPgo);
    m_inspect->setEnabled(!busy && hasGame);
    const bool maintenance = busy && (m_activeAction == "clean" || m_activeAction == "tools");
    const bool builds = m_compilations.activeCount() || m_compilations.pendingCount();
    m_compile->setEnabled(!maintenance && hasGame);
    m_buildTools->setEnabled(!busy && !builds);
    m_clean->setEnabled(!busy && !builds);

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
        m_compileAction->setEnabled(!maintenance && hasGame);
    if (m_pgoAction)
        m_pgoAction->setEnabled(canPgo);
    if (m_buildToolsAction)
        m_buildToolsAction->setEnabled(!busy && !builds);
    if (m_cleanAction)
        m_cleanAction->setEnabled(!busy && !builds);
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
    QStringList paths;
    for (const QUrl& url : event->mimeData()->urls())
    {
        if (url.isLocalFile() && looksLikeDiscImage(url.toLocalFile()))
        {
            paths << url.toLocalFile();
        }
    }
    if (paths.isEmpty()) return;
    if (paths.size() == 1 && !m_controller.isRunning()) setIsoPath(paths.first(), true);
    else enqueueCompilations(paths);
    event->acceptProposedAction();
}

void MainWindow::updateApplicationIcon()
{
    QIcon icon=shellProjectIcon();
    if(m_runtimeStarted && m_runtimeEmbedded && QSettings("GekkoAOT","GekkoAOT").value("ui/gameAppIcon",true).toBool()) {
        for(const auto& entry:m_library->entries())if(entry.path==m_isoPath){icon=shellGameIcon(entry.banner);break;}
    }
    setWindowIcon(m_runtimeOnDesktop?shellProjectIcon():icon);
    QIcon gameIcon=shellProjectIcon();
    if(QSettings("GekkoAOT","GekkoAOT").value("ui/gameAppIcon",true).toBool())for(const auto& entry:m_library->entries())if(entry.path==m_isoPath){gameIcon=shellGameIcon(entry.banner);break;}
    const QString gameId=m_library->metadata(m_isoPath)["gameId"].toString();
    setShellWindowIdentity(this,{},"GekkoAOT",shellProjectIcon());
    setShellWindowIdentity(m_gameWindow,gameId.isEmpty()?"unknown":gameId,gameTitleForPath(m_isoPath),gameIcon);
    qApp->setWindowIcon(shellProjectIcon());
}
void MainWindow::showAppearanceConfig()
{
    QDialog dialog(this);dialog.setWindowTitle("Appearance");dialog.resize(640,420);
    auto* layout=new QVBoxLayout(&dialog);layout->setContentsMargins(24,24,24,24);layout->setSpacing(18);
    QSettings config("GekkoAOT","GekkoAOT");
    auto* brand=new QCheckBox("Show project logo in Desktop and TV Mode");brand->setObjectName("showProjectLogo");brand->setChecked(config.value("ui/showProjectLogo",true).toBool());layout->addWidget(brand);
    auto* game=new QCheckBox("Use the running game's banner as its application icon");game->setObjectName("useGameAppIcon");game->setChecked(config.value("ui/gameAppIcon",true).toBool());layout->addWidget(game);
    auto* motion=new QCheckBox("Reduce interface animations");motion->setChecked(config.value("ui/reducedMotion",false).toBool());layout->addWidget(motion);
    auto* preview=new QLabel;preview->setPixmap(shellProjectIcon().pixmap(72,72));preview->setAlignment(Qt::AlignCenter);layout->addWidget(preview);
    QString path=config.value("ui/projectLogo").toString();
    auto* choose=new QPushButton(shellIcon("folder-open"),"Choose project logo…");layout->addWidget(choose);
    connect(choose,&QPushButton::clicked,&dialog,[&]{
        QFileDialog files(&dialog,"Project logo");files.setOption(QFileDialog::DontUseNativeDialog);files.setFileMode(QFileDialog::ExistingFile);files.setNameFilter("Images (*.png *.jpg *.jpeg *.bmp *.ico)");presentShellDialog(files,"Project logo",m_shellVisible);
        if(execShellDialog(files)==QDialog::Accepted && !files.selectedFiles().isEmpty()){QImage image(files.selectedFiles().first());if(!image.isNull()){path=files.selectedFiles().first();preview->setPixmap(QPixmap::fromImage(image).scaled(72,72,Qt::KeepAspectRatio,Qt::SmoothTransformation));}}
    });
    auto* reset=new QPushButton("Restore GekkoAOT logo");layout->addWidget(reset);connect(reset,&QPushButton::clicked,&dialog,[&]{path.clear();preview->setPixmap(QPixmap(":/launcher/app-icon.png").scaled(72,72));});
    auto* licences=new QPushButton("Google Sans Flex · licence");layout->addWidget(licences);
    connect(licences,&QPushButton::clicked,&dialog,[&]{
        QDialog notice(&dialog);auto* body=new QVBoxLayout(&notice);auto* text=new QPlainTextEdit;text->setReadOnly(true);
        QFile file(":/launcher/fonts/GoogleSansFlex-OFL.txt");if(file.open(QIODevice::ReadOnly))text->setPlainText(QString::fromUtf8(file.readAll()));body->addWidget(text);
        presentShellDialog(notice,"Font licence",m_shellVisible);execShellDialog(notice);
    });
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);layout->addWidget(buttons);connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    presentShellDialog(dialog,"Appearance",m_shellVisible);
    if(execShellDialog(dialog)!=QDialog::Accepted)return;
    config.setValue("ui/projectLogo",path);config.setValue("ui/showProjectLogo",brand->isChecked());config.setValue("ui/gameAppIcon",game->isChecked());config.setValue("ui/reducedMotion",motion->isChecked());
    auto* logo=findChild<QLabel*>("desktopProjectLogo");logo->setPixmap(shellProjectIcon().pixmap(36,36));logo->setVisible(brand->isChecked());
    m_livingRoom->refreshBrand();m_livingRoom->setReducedMotion(motion->isChecked());updateApplicationIcon();
}
void MainWindow::refreshDiscordPresence()
{
    DiscordPresenceContext context;
    const auto publicTitle = [this](const QString& path) {
        if (!m_discordTitles.contains(path))
            m_discordTitles.insert(path, readDiscMetadata(discMetadataCachePath(runtimeStateRoot(), path))["title"].toString());
        return m_discordTitles.value(path);
    };
    context.gameRunning = m_runtimeStarted;
    context.gameTitle = m_runtimeStarted ? publicTitle(m_isoPath) : QString{};
    context.gameStartedAt = m_discordGameStart;
    context.paused = m_paused;
    context.tvMode = m_returnToTV;
    if (!context.gameRunning) {
        for (const auto id : m_compilations.ids()) {
            const auto* job = m_compilations.job(id);
            if (job && job->state == CompilationJob::Running) {
                context.compiling = true;
                context.compilationTitle = publicTitle(job->request.isoPath);
                break;
            }
        }
    }
    m_discord.setActivity(discordActivityFor(context));
}

void MainWindow::showDiscordConfig()
{
    QDialog dialog(this);
    dialog.setObjectName("discordSettings");
    dialog.resize(650, 430);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24,24,24,24);
    layout->setSpacing(18);
    QSettings config("GekkoAOT", "GekkoAOT");
    auto* enabled = new QCheckBox("Share activity with Discord");
    enabled->setObjectName("discordEnabled");
    enabled->setChecked(config.value("discord/enabled", true).toBool());
    layout->addWidget(enabled);
    auto* description = mutedLabel("Show the current game, pause or compilation on your Discord profile. Turn this off at any time.", &dialog);
    description->setWordWrap(true); layout->addWidget(description);
    auto* form = new QFormLayout;
    auto* id = new QLineEdit(config.value("discord/applicationId", DiscordPresence::defaultApplicationId()).toString());
    id->setObjectName("discordApplicationId");
    id->setAccessibleName("Discord Application ID");
    id->setPlaceholderText("Public application ID");
    id->setMaxLength(20);
    id->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,20}"), id));
    form->addRow("Application ID", id); layout->addLayout(form);
    auto* status = new QLabel(m_discord.statusText());
    status->setObjectName("discordStatus"); status->setWordWrap(true);
    layout->addWidget(status);
    connect(&m_discord, &DiscordPresence::statusChanged, &dialog, [this, status] { status->setText(m_discord.statusText()); });
    layout->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (enabled->isChecked() && !DiscordPresence::validApplicationId(id->text())) {
            status->setText("Enter the public Application ID from the Discord Developer Portal.");
            id->setFocus(); return;
        }
        config.setValue("discord/enabled", enabled->isChecked());
        if (id->text().trimmed().isEmpty()) config.remove("discord/applicationId");
        else config.setValue("discord/applicationId", id->text().trimmed());
        refreshDiscordPresence();
        m_discord.configure(enabled->isChecked(), id->text());
        dialog.accept();
    });
    presentShellDialog(dialog, "Discord", m_shellVisible);
    execShellDialog(dialog);
}

void MainWindow::showCompilations()
{
    if(auto* existing=findChild<QDialog*>("compilationQueuePanel")) {
        if(existing->property("tvMode").toBool()!=m_shellVisible)presentShellDialog(*existing,"Compilations",m_shellVisible);
        existing->show();existing->raise();existing->activateWindow();return;
    }
    auto* dialog=new QDialog(this);dialog->setObjectName("compilationQueuePanel");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle("Compilations");dialog->resize(520,qMin(700,screen()->availableGeometry().height()-80));
    auto* layout=new QVBoxLayout(dialog);auto* view=new CompilationSidebar(&m_compilations,dialog);view->compileRequested=[this](const QStringList& paths){enqueueCompilations(paths);};view->playRequested=[this](const QString& path){launchCompiledGame(path);};layout->addWidget(view);
    auto* close=new QPushButton("Close");layout->addWidget(close);connect(close,&QPushButton::clicked,dialog,&QDialog::close);
    presentShellDialog(*dialog,"Compilations",m_shellVisible);dialog->show();dialog->raise();dialog->findChild<QPushButton*>("shellDialogBack")->setFocus();updateShellInput();
}

void MainWindow::launchCompiledGame(const QString& path)
{
    if(auto* panel=shellActiveDialog(this))panel->close();
    if(m_shellVisible)m_livingRoom->playRequested(path);
    else if(m_library->activated)m_library->activated(path);
}

void MainWindow::showNavigationPanel(bool settingsOnly)
{
    if(auto* old=shellActiveDialog(this)) {
        const bool same=old->objectName()=="navigationPanel";
        old->close();if(same)return;
    }
    auto* dialog=new QDialog(this);dialog->setObjectName("navigationPanel");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->resize(500,620);
    auto* layout=new QVBoxLayout(dialog);layout->setContentsMargins(8,8,8,8);layout->setSpacing(4);
    auto add=[&](const QString& text,const QString& icon,std::function<void()> action,bool enabled=true){
        auto* button=new QPushButton(shellIcon(icon),text);button->setIconSize({19,19});button->setFixedHeight(m_shellVisible?50:46);
        button->setStyleSheet("QPushButton {text-align:left;padding:0 12px;border:1px solid transparent;border-radius:11px;background:transparent;}QPushButton:hover {background:rgba(255,255,255,15);}QPushButton:focus {background:rgba(255,255,255,18);border-color:rgba(255,255,255,100);}QPushButton:pressed {background:rgba(255,255,255,26);}");button->setEnabled(enabled);layout->addWidget(button);
        connect(button,&QPushButton::clicked,dialog,[dialog,action]{dialog->close();QTimer::singleShot(0,qApp,action);});
    };
    add("Open game…","folder-open",[this]{m_openAction->trigger();},m_openAction->isEnabled());
    add("Graphics","sliders-horizontal",[this]{showGraphicsConfig();});
    add("Controllers","gamepad-2",[this]{showInputConfig();});
    add("Memory cards","memory-stick",[this]{showMemoryCards();});
    auto* divider=new QFrame;divider->setFixedHeight(1);divider->setStyleSheet("background:rgba(255,255,255,18);margin:4px 8px;");layout->addWidget(divider);
    add("Appearance","sparkles",[this]{showAppearanceConfig();});
    add("Discord","messages-square",[this]{showDiscordConfig();});
    add("Advanced","settings-2",[this]{setAdvancedVisible(true);});
    presentShellDialog(*dialog,settingsOnly?"Settings":"Menu",m_shellVisible);dialog->show();dialog->raise();
    if(auto* first=dialog->findChild<QPushButton*>())first->setFocus();
}

void MainWindow::showMemoryCards()
{
    if(auto* panel=shellActiveDialog(this))panel->close();
    QMap<QString,QString> titles;for(const auto& game:m_library->entries()){if(!game.gameId.isEmpty())titles[game.gameId]=game.title;titles[slugForPath(game.path)]=game.title;}
    MemoryCardManager dialog(runtimeStateRoot(),frontendUserDir(),m_runtimeStarted?frontendUserDir():QString{},titles,this);
    presentShellDialog(dialog,"Memory cards",m_shellVisible);execShellDialog(dialog);
}
