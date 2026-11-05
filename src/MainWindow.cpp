// SPDX-License-Identifier: GPL-3.0-or-later
// Main window: construction, menus, signal wiring and settings persistence.
#include "MainWindowInternal.hpp"

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


    // The performance HUD is drawn by the renderer into the game image
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

    m_compileMode = new QComboBox(m_advancedPanel);
    m_compileMode->addItem("Full optimization (slowest compile, best performance)", "full");
    m_compileMode->addItem("Balanced (no IR passes, optimized code generation)", "balanced");
    m_compileMode->addItem("Fast start (fastest compile, code about 3x slower)", "fast");
    m_compileMode->setToolTip(
        "How a game is compiled the first time. Medal of Honor: Frontline's main "
        "executable: about 10 minutes, 5 minutes and under 1 minute. Each mode keeps "
        "its own cache, so switching back does not recompile.");

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
    settingsForm->addRow("Compilation", m_compileMode);
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
    connect(m_livingRoom, &LivingRoomView::texturePacksRequested, this, &MainWindow::showTexturePacks);
    connect(m_livingRoom, &LivingRoomView::cheatCodesRequested, this, &MainWindow::showCheatCodes);
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
        m_compileMode->setEnabled(llvm);
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
    // The unlock re-renders geometry. The old image-blend setting
    // ("graphics/frameInterpolation") is deliberately not migrated.
    m_frameInterpolation = s.value("graphics/frameUnlock", false).toBool();
    m_graphicsExtras.unlock_predict = s.value("graphics/frameUnlockPredict", false).toBool() ? 1u : 0u;
    // Interpolation is the only mode that animates CPU-built meshes (and never
    // overshoots): settings saved before it did switch back to it once.
    if (s.value("graphics/frameUnlockModeVersion", 0).toInt() < 1) m_graphicsExtras.unlock_predict = 0;
    m_graphicsExtras.vsync = s.value("graphics/vsync", false).toBool() ? 1u : 0u;
    m_fpsLimit = qBound(1, s.value("graphics/fpsLimit", 120).toInt(), 1000);
    m_msaaSamples = s.value("graphics/msaaSamples", 1).toInt() == 4 ? 4 : 1;
    const int anisotropy = s.value("graphics/textureAnisotropy", 16).toInt();
    m_textureAnisotropy = (anisotropy == 1 || anisotropy == 2 || anisotropy == 4 ||
                           anisotropy == 8 || anisotropy == 16) ? anisotropy : 16;
    m_graphicsExtras.render_percent = qBound(25, s.value("graphics/renderPercent", 100).toInt(), 400);
    m_graphicsExtras.fxaa = s.value("graphics/fxaa", false).toBool();
    m_graphicsExtras.fragment_lighting = s.value("graphics/fragment_lighting", true).toBool();
    m_graphicsExtras.fov_y = s.value("graphics/fovY", 0).toFloat();
    if(!std::isfinite(m_graphicsExtras.fov_y) || (m_graphicsExtras.fov_y!=0 && (m_graphicsExtras.fov_y<20 || m_graphicsExtras.fov_y>150))) m_graphicsExtras.fov_y=0;
    m_graphicsExtras.brightness = qBound(0.0, s.value("graphics/brightness", double(m_graphicsExtras.brightness)).toDouble(), 2.0);
    m_graphicsExtras.contrast = qBound(0.0, s.value("graphics/contrast", double(m_graphicsExtras.contrast)).toDouble(), 2.0);
    m_graphicsExtras.saturation = qBound(0.0, s.value("graphics/saturation", double(m_graphicsExtras.saturation)).toDouble(), 2.0);
    m_graphicsExtras.gamma = qBound(0.5, s.value("graphics/gamma", double(m_graphicsExtras.gamma)).toDouble(), 2.5);
    m_graphicsExtras.exposure = qBound(-3.0, s.value("graphics/exposure", double(m_graphicsExtras.exposure)).toDouble(), 3.0);
    m_graphicsExtras.temperature = qBound(-1.0, s.value("graphics/temperature", double(m_graphicsExtras.temperature)).toDouble(), 1.0);
    m_graphicsExtras.vibrance = qBound(0.0, s.value("graphics/vibrance", double(m_graphicsExtras.vibrance)).toDouble(), 1.0);
    m_graphicsExtras.tint = qBound(-1.0, s.value("graphics/tint", double(m_graphicsExtras.tint)).toDouble(), 1.0);
    m_graphicsExtras.enhanced = qBound(0, s.value("graphics/enhanced_renderer", int(m_graphicsExtras.enhanced)).toInt(), 1);
    m_graphicsExtras.ao_quality = qBound(0, s.value("graphics/ao_quality", int(m_graphicsExtras.ao_quality)).toInt(), 3);
    m_graphicsExtras.contact_shadows = qBound(0, s.value("graphics/contact_shadows", int(m_graphicsExtras.contact_shadows)).toInt(), 1);
    m_graphicsExtras.ao_strength = qBound(0.0, s.value("graphics/ao_strength", double(m_graphicsExtras.ao_strength)).toDouble(), 2.0);
    m_graphicsExtras.ao_radius = qBound(0.01, s.value("graphics/ao_radius", double(m_graphicsExtras.ao_radius)).toDouble(), 1000.0);
    m_graphicsExtras.shadow_strength = qBound(0.0, s.value("graphics/shadow_strength", double(m_graphicsExtras.shadow_strength)).toDouble(), 1.0);
    m_graphicsExtras.shadow_distance = qBound(0.01, s.value("graphics/shadow_distance", double(m_graphicsExtras.shadow_distance)).toDouble(), 10000.0);
    m_graphicsExtras.shadow_thickness = qBound(0.001, s.value("graphics/shadow_thickness", double(m_graphicsExtras.shadow_thickness)).toDouble(), 100.0);
    m_graphicsExtras.shadow_softness = qBound(0.0, s.value("graphics/shadow_softness", double(m_graphicsExtras.shadow_softness)).toDouble(), 1.0);
    m_graphicsExtras.light_x = qBound(-1.0, s.value("graphics/light_x", double(m_graphicsExtras.light_x)).toDouble(), 1.0);
    m_graphicsExtras.light_y = qBound(-1.0, s.value("graphics/light_y", double(m_graphicsExtras.light_y)).toDouble(), 1.0);
    m_graphicsExtras.light_z = qBound(-1.0, s.value("graphics/light_z", double(m_graphicsExtras.light_z)).toDouble(), 1.0);
    m_graphicsExtras.environment = qBound(0, s.value("graphics/environment_lighting", int(m_graphicsExtras.environment)).toInt(), 2);
    m_graphicsExtras.sun_preset = qBound(0, s.value("graphics/sun_preset", int(m_graphicsExtras.sun_preset)).toInt(), 3);
    m_graphicsExtras.environment_strength = qBound(0.0, s.value("graphics/environment_strength", double(m_graphicsExtras.environment_strength)).toDouble(), 1.0);
    m_graphicsExtras.sun_intensity = qBound(0.0, s.value("graphics/sun_intensity", double(m_graphicsExtras.sun_intensity)).toDouble(), 4.0);
    m_graphicsExtras.sun_temperature = qBound(1500.0, s.value("graphics/sun_temperature", double(m_graphicsExtras.sun_temperature)).toDouble(), 12000.0);
    m_graphicsExtras.sun_azimuth = qBound(-360.0, s.value("graphics/sun_azimuth", double(m_graphicsExtras.sun_azimuth)).toDouble(), 360.0);
    m_graphicsExtras.sun_elevation = qBound(1.0, s.value("graphics/sun_elevation", double(m_graphicsExtras.sun_elevation)).toDouble(), 90.0);
    m_graphicsExtras.sky_intensity = qBound(0.0, s.value("graphics/sky_intensity", double(m_graphicsExtras.sky_intensity)).toDouble(), 2.0);
    m_graphicsExtras.sun_shadows = qBound(0, s.value("graphics/sun_shadows", int(m_graphicsExtras.sun_shadows)).toInt(), 2);
    m_graphicsExtras.gi = qBound(0, s.value("graphics/gi", int(m_graphicsExtras.gi)).toInt(), 1);
    m_graphicsExtras.ssr = qBound(0, s.value("graphics/ssr", int(m_graphicsExtras.ssr)).toInt(), 1);
    m_graphicsExtras.gi_strength = qBound(0.0, s.value("graphics/gi_strength", double(m_graphicsExtras.gi_strength)).toDouble(), 2.0);
    m_graphicsExtras.ssr_strength = qBound(0.0, s.value("graphics/ssr_strength", double(m_graphicsExtras.ssr_strength)).toDouble(), 4.0);

    for(const auto* key : {"particle_lights", "particle_light_strength", "particle_light_radius", "dof_quality", "dof_strength", "dof_focus", "texture_scale", "texture_quality", "texture_backend", "hd_budget_mib", "hd_cache_mib", "surface_detail", "surface_specular", "material_enhancement", "material_compare", "geometry_refinement", "displacement", "pom_quality", "material_normal_strength", "material_relief"})
        if(s.contains(QString("graphics/")+key))
            GekkoAOT::GX::ParseGraphicsSetting(m_graphicsExtras,key,s.value(QString("graphics/")+key).toString().toStdString());

    m_postProcessing = s.value("graphics/postProcessing", true).toBool();
    m_postPreset = s.value("graphics/postPreset", "balanced").toString();
    if (m_postPreset != "balanced" && m_postPreset != "cinematic" &&
        m_postPreset != "crisp" && m_postPreset != "neutral" && m_postPreset != "custom" &&
        m_postPreset != "remastered" && m_postPreset != "remastered_vivid" &&
        m_postPreset != "remastered_cinematic" && m_postPreset != "remastered_performance" &&
        m_postPreset != "remastered_remake")
        m_postPreset = "balanced";
    m_postSharpen = qBound(0.0, s.value("graphics/postSharpen", 0.28).toDouble(), 1.0);
    m_postBloom = qBound(0.0, s.value("graphics/postBloom", 0.10).toDouble(), 1.0);
    m_postVignette = qBound(0.0, s.value("graphics/postVignette", 0.06).toDouble(), 1.0);
    m_postGrain = qBound(0.0, s.value("graphics/postGrain", 0.0).toDouble(), 0.25);
    m_postTone = qBound(0.0, s.value("graphics/postTone", 0.15).toDouble(), 1.0);
    m_postLocalContrast = qBound(0.0, s.value("graphics/postLocalContrast", 0.08).toDouble(), 1.0);
    // Remastered is the default look: a fresh install starts with it, and
    // settings saved before it existed switch to it once. Later choices stay.
    if (s.value("graphics/remasterDefaults", 0).toInt() < 1) {
        GekkoAOT::GX::ApplyGraphicsPreset(m_graphicsExtras, "remastered");
        m_postPreset = "remastered";
        m_postProcessing = m_graphicsExtras.post != 0;
        m_postSharpen = m_graphicsExtras.sharpen; m_postBloom = m_graphicsExtras.bloom;
        m_postVignette = m_graphicsExtras.vignette; m_postGrain = m_graphicsExtras.grain;
        m_postTone = m_graphicsExtras.tone; m_postLocalContrast = m_graphicsExtras.local_contrast;
    }
    // Curved meshes left the presets: values saved by a preset go once, a later
    // explicit choice stays.
    if (s.value("graphics/curvedMeshOff", 0).toInt() < 1)
        m_graphicsExtras.geometry_refinement = 0;
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
    restoreCombo(m_compileMode, "compileMode", "full");
    restoreCombo(m_runtimeWorkers, "advanced/runtimeWorkers", "parallel");
    // NativeOS is part of the validated low-CPU configuration (idle/spin
    // parking, SDK services); a fresh install starts with it enabled.
    m_nativeOsCheck->setChecked(s.value("advanced/nativeOS", true).toBool());
    m_nativeHwCheck->setChecked(s.value("advanced/nativeHW", true).toBool());
    m_nativeAbi->setEnabled(m_backend->currentData().toString() == "llvm");
    m_compileMode->setEnabled(m_backend->currentData().toString() == "llvm");
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
    s.setValue("compileMode", m_compileMode->currentData());
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
    s.setValue("graphics/frameUnlockModeVersion", 1);
    s.setValue("graphics/vsync", m_graphicsExtras.vsync != 0);
    s.setValue("graphics/hudItems", m_hudItems);
    s.setValue("graphics/fpsLimit", m_fpsLimit);
    s.setValue("graphics/msaaSamples", m_msaaSamples);
    s.setValue("graphics/textureAnisotropy", m_textureAnisotropy);
    s.setValue("graphics/renderPercent", int(m_graphicsExtras.render_percent));
    s.setValue("graphics/fxaa", bool(m_graphicsExtras.fxaa));
    s.setValue("graphics/fragment_lighting", m_graphicsExtras.fragment_lighting);
    s.setValue("graphics/fovY", m_graphicsExtras.fov_y);
    s.setValue("graphics/brightness", m_graphicsExtras.brightness);
    s.setValue("graphics/contrast", m_graphicsExtras.contrast);
    s.setValue("graphics/saturation", m_graphicsExtras.saturation);
    s.setValue("graphics/gamma", m_graphicsExtras.gamma);
    s.setValue("graphics/exposure", m_graphicsExtras.exposure);
    s.setValue("graphics/temperature", m_graphicsExtras.temperature);
    s.setValue("graphics/vibrance", m_graphicsExtras.vibrance);
    s.setValue("graphics/tint", m_graphicsExtras.tint);
    s.setValue("graphics/enhanced_renderer", m_graphicsExtras.enhanced);
    s.setValue("graphics/ao_quality", m_graphicsExtras.ao_quality);
    s.setValue("graphics/contact_shadows", m_graphicsExtras.contact_shadows);
    s.setValue("graphics/ao_strength", m_graphicsExtras.ao_strength);
    s.setValue("graphics/ao_radius", m_graphicsExtras.ao_radius);
    s.setValue("graphics/shadow_strength", m_graphicsExtras.shadow_strength);
    s.setValue("graphics/shadow_distance", m_graphicsExtras.shadow_distance);
    s.setValue("graphics/shadow_thickness", m_graphicsExtras.shadow_thickness);
    s.setValue("graphics/shadow_softness", m_graphicsExtras.shadow_softness);
    s.setValue("graphics/light_x", m_graphicsExtras.light_x);
    s.setValue("graphics/light_y", m_graphicsExtras.light_y);
    s.setValue("graphics/light_z", m_graphicsExtras.light_z);
    s.setValue("graphics/environment_lighting", m_graphicsExtras.environment);
    s.setValue("graphics/sun_preset", m_graphicsExtras.sun_preset);
    s.setValue("graphics/environment_strength", m_graphicsExtras.environment_strength);
    s.setValue("graphics/sun_intensity", m_graphicsExtras.sun_intensity);
    s.setValue("graphics/sun_temperature", m_graphicsExtras.sun_temperature);
    s.setValue("graphics/sun_azimuth", m_graphicsExtras.sun_azimuth);
    s.setValue("graphics/sun_elevation", m_graphicsExtras.sun_elevation);
    s.setValue("graphics/sky_intensity", m_graphicsExtras.sky_intensity);
    s.setValue("graphics/sun_shadows", m_graphicsExtras.sun_shadows);
    s.setValue("graphics/gi", m_graphicsExtras.gi);
    s.setValue("graphics/particle_lights", m_graphicsExtras.particle_lights);
    s.setValue("graphics/particle_light_strength", m_graphicsExtras.particle_light_strength);
    s.setValue("graphics/particle_light_radius", m_graphicsExtras.particle_light_radius);
    s.setValue("graphics/dof_quality", m_graphicsExtras.dof_quality);
    s.setValue("graphics/dof_strength", m_graphicsExtras.dof_strength);
    s.setValue("graphics/dof_focus", m_graphicsExtras.dof_focus);
    s.setValue("graphics/ssr", m_graphicsExtras.ssr);
    s.setValue("graphics/gi_strength", m_graphicsExtras.gi_strength);
    s.setValue("graphics/ssr_strength", m_graphicsExtras.ssr_strength);
    s.setValue("graphics/texture_scale", m_graphicsExtras.texture_scale);
    s.setValue("graphics/texture_quality", m_graphicsExtras.texture_quality);
    s.setValue("graphics/texture_backend", m_graphicsExtras.texture_backend);
    s.setValue("graphics/hd_budget_mib", m_graphicsExtras.hd_budget_mib);
    s.setValue("graphics/hd_cache_mib", m_graphicsExtras.hd_cache_mib);
    s.setValue("graphics/material_enhancement", m_graphicsExtras.material_enhancement);
    s.setValue("graphics/material_compare", m_graphicsExtras.material_compare);
    s.setValue("graphics/geometry_refinement", m_graphicsExtras.geometry_refinement);
    s.setValue("graphics/displacement", m_graphicsExtras.displacement);
    s.setValue("graphics/pom_quality", m_graphicsExtras.pom_quality);
    s.setValue("graphics/material_normal_strength", m_graphicsExtras.material_normal_strength);
    s.setValue("graphics/material_relief", m_graphicsExtras.material_relief);
    s.setValue("graphics/surface_detail", m_graphicsExtras.surface_detail);
    s.setValue("graphics/surface_specular", m_graphicsExtras.surface_specular);

    s.setValue("graphics/postProcessing", m_postProcessing);
    s.setValue("graphics/postPreset", m_postPreset);
    s.setValue("graphics/postSharpen", m_postSharpen);
    s.setValue("graphics/postBloom", m_postBloom);
    s.setValue("graphics/postVignette", m_postVignette);
    s.setValue("graphics/postGrain", m_postGrain);
    s.setValue("graphics/postTone", m_postTone);
    s.setValue("graphics/postLocalContrast", m_postLocalContrast);
    s.setValue("graphics/remasterDefaults", 1); // saved together with the migrated values
    s.setValue("graphics/curvedMeshOff", 1);
    s.setValue("input/controllers", m_controllers);
}
