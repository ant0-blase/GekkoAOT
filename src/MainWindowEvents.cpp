// SPDX-License-Identifier: GPL-3.0-or-later
// Main window: window events, drag and drop, compilations and mod managers.
#include "MainWindowInternal.hpp"

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
    m_compileMode->setEnabled(m_backend->currentData().toString() == "llvm");
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
    add("Texture packs","library",[this]{showTexturePacks();});
    add("Cheat codes","file-text",[this]{showCheatCodes();});
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


void MainWindow::showCheatCodes()
{
    if (auto* panel = shellActiveDialog(this)) panel->close();
    QMap<QString,QString> titles;
    for (const auto& game : m_library->entries())
        if (!game.gameId.isEmpty()) titles[game.gameId.toUpper()] = game.title;
    const QString id = discIdForPath(m_shellVisible && !m_livingRoom->currentGame().isEmpty()
                                              ? m_livingRoom->currentGame() : m_isoPath).toUpper();
    if (!id.isEmpty() && !titles.contains(id)) titles[id] = gameTitleForPath(m_isoPath);
    const QString root = qEnvironmentVariable("GEKKOAOT_MOD_ROOT",
                                QDir(runtimeResourceRoot()).filePath("mods"));
    ModManagerDialog dialog(ModManagerDialog::Mode::CheatCodes, root, titles, id,
                            runtimeStateRoot(), this);
    presentShellDialog(dialog, "Cheat codes", m_shellVisible);
    execShellDialog(dialog);
}

void MainWindow::showTexturePacks()
{
    if (auto* panel = shellActiveDialog(this)) panel->close();
    QMap<QString,QString> titles;
    for (const auto& game : m_library->entries())
        if (!game.gameId.isEmpty()) titles[game.gameId.toUpper()] = game.title;
    const QString id = discIdForPath(m_shellVisible && !m_livingRoom->currentGame().isEmpty()
                                              ? m_livingRoom->currentGame() : m_isoPath).toUpper();
    if (!id.isEmpty() && !titles.contains(id)) titles[id] = gameTitleForPath(m_isoPath);
    const QString root = qEnvironmentVariable("GEKKOAOT_MOD_ROOT",
                                QDir(runtimeResourceRoot()).filePath("mods"));
    ModManagerDialog dialog(ModManagerDialog::Mode::TexturePacks, root, titles, id,
                            runtimeStateRoot(), this);
    presentShellDialog(dialog, "Texture packs", m_shellVisible);
    execShellDialog(dialog);
}
