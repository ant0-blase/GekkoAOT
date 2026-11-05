// SPDX-License-Identifier: GPL-3.0-or-later
// Main window: graphics, log and launcher dialogs.
#include "MainWindowInternal.hpp"

void MainWindow::showGraphicsConfig()
{
    GraphicsOptions options;
    options.image=m_graphicsExtras;
    options.image.msaa=m_msaaSamples; options.image.anisotropy=m_textureAnisotropy;
    options.image.post=m_postProcessing; options.image.interpolation=m_frameInterpolation;
    options.image.sharpen=m_postSharpen; options.image.bloom=m_postBloom;
    options.image.vignette=m_postVignette; options.image.grain=m_postGrain;
    options.image.tone=m_postTone; options.image.local_contrast=m_postLocalContrast;
    options.image.cinematic=m_postPreset=="cinematic"?.20f:m_postPreset=="remastered_cinematic" || m_postPreset=="remastered_remake"?.10f:0.f;
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
    const auto appliedStatus=[this]{
        QFile state(QDir(frontendUserDir()).filePath("graphics-applied.json"));
        if(!state.open(QIODevice::ReadOnly))return QString("Waiting for renderer acknowledgement…");
        const auto info=QJsonDocument::fromJson(state.readAll()).object();
        if(quint64(info.value("revision").toDouble())!=m_graphicsRevision)return QString("Applying settings…");
        if(!info.value("accepted").toBool())return QString("Renderer rejected these settings; previous settings remain active.");
        if(info.value("pending_status_known").toBool() && info.value("pending_texture_bytes").toDouble()>0)return QString("Applied · texture reconstruction preparing in the background");
        return QString("Applied · cached resources ready when available");
    };
    if(editGraphicsOptions(this,options,m_runtimeStarted,preview,appliedStatus)==QDialog::Accepted) {
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
    request.compileMode = m_compileMode->currentData().toString();
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
    // The managers and the spawned controller must agree on the same mods
    // root even in installed/portable builds whose working dir differs.
    if (!request.gameEnvironment.contains("GEKKOAOT_MOD_ROOT"))
        request.gameEnvironment.insert("GEKKOAOT_MOD_ROOT", qEnvironmentVariable(
            "GEKKOAOT_MOD_ROOT", QDir(runtimeResourceRoot()).filePath("mods")));
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
