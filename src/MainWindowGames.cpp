// SPDX-License-Identifier: GPL-3.0-or-later
// Main window: game selection, disc identity and metadata.
#include "MainWindowInternal.hpp"

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

    m_graphicsRevision=std::max(m_graphicsRevision+1,quint64(QDateTime::currentMSecsSinceEpoch()));
    QTextStream out(&file);
    out << "# Generated by GekkoAOT\n"
           "# GekkoAOT native frontend settings\n"
           "[Emulation]\n"
        << "paused=" << (m_paused ? "true" : "false") << "\n"
        << "[Video]\n"
        << "graphics_revision=" << m_graphicsRevision << "\n"
        << "backend=" << m_graphicsBackend << "\n"
        << "aspect_ratio=" << m_aspectMode << "\n"
        << "fullscreen=" << (m_fullscreen ? "true" : "false") << "\n"
        << "show_fps_in_title=" << (m_showFps ? "true" : "false") << "\n"
        << "fps_limit=" << m_fpsLimit << "\n"
        << "frame_interpolation=" << (m_frameInterpolation ? 1 : 0) << "\n"
        << "frame_unlock_predict=" << m_graphicsExtras.unlock_predict << "\n"
        << "vsync=" << m_graphicsExtras.vsync << "\n"
        << "hud=" << (m_showFps ? 1 : 0) << "\n";
    for (const auto& item : GekkoAOT::GX::HudItemKeys)
        out << item.key << "=" << ((m_hudItems & item.bit) ? 1 : 0) << "\n";
    out
        << "render_percent=" << m_graphicsExtras.render_percent << "\n"
        << "render_width=" << (screen()->size() * screen()->devicePixelRatio()).width() << "\n"
        << "render_height=" << (screen()->size() * screen()->devicePixelRatio()).height() << "\n"
        << "fxaa=" << m_graphicsExtras.fxaa << "\n"
        << "fov_y=" << m_graphicsExtras.fov_y << "\n"
        << "post_processing=" << (m_postProcessing ? 1 : 0) << "\n"
        << "post_sharpen=" << m_postSharpen << "\n"
        << "post_bloom=" << m_postBloom << "\n"
        << "post_vignette=" << m_postVignette << "\n"
        << "post_grain=" << m_postGrain << "\n"
        << "post_tone=" << m_postTone << "\n"
        << "post_local_contrast=" << m_postLocalContrast << "\n"
        << "post_cinematic=" << (m_postPreset == "cinematic" ? .20 : m_postPreset == "remastered_cinematic" || m_postPreset == "remastered_remake" ? .10 : 0.) << "\n"
        << "post_brightness=" << m_graphicsExtras.brightness << "\n"
        << "post_contrast=" << m_graphicsExtras.contrast << "\n"
        << "post_saturation=" << m_graphicsExtras.saturation << "\n"
        << "post_gamma=" << m_graphicsExtras.gamma << "\n"
        << "post_exposure=" << m_graphicsExtras.exposure << "\n"
        << "post_temperature=" << m_graphicsExtras.temperature << "\n"
        << "post_vibrance=" << m_graphicsExtras.vibrance << "\n"
        << "post_tint=" << m_graphicsExtras.tint << "\n"
        << "enhanced_renderer=" << m_graphicsExtras.enhanced << "\n"
        << "ao_quality=" << m_graphicsExtras.ao_quality << "\n"
        << "fragment_lighting=" << m_graphicsExtras.fragment_lighting << "\n"
        << "contact_shadows=" << m_graphicsExtras.contact_shadows << "\n"
        << "ao_strength=" << m_graphicsExtras.ao_strength << "\n"
        << "ao_radius=" << m_graphicsExtras.ao_radius << "\n"
        << "shadow_strength=" << m_graphicsExtras.shadow_strength << "\n"
        << "shadow_distance=" << m_graphicsExtras.shadow_distance << "\n"
        << "shadow_thickness=" << m_graphicsExtras.shadow_thickness << "\n"
        << "shadow_softness=" << m_graphicsExtras.shadow_softness << "\n"
        << "light_x=" << m_graphicsExtras.light_x << "\n"
        << "light_y=" << m_graphicsExtras.light_y << "\n"
        << "light_z=" << m_graphicsExtras.light_z << "\n"
        << "environment_lighting=" << m_graphicsExtras.environment << "\n"
        << "sun_preset=" << m_graphicsExtras.sun_preset << "\n"
        << "environment_strength=" << m_graphicsExtras.environment_strength << "\n"
        << "sun_intensity=" << m_graphicsExtras.sun_intensity << "\n"
        << "sun_temperature=" << m_graphicsExtras.sun_temperature << "\n"
        << "sun_azimuth=" << m_graphicsExtras.sun_azimuth << "\n"
        << "sun_elevation=" << m_graphicsExtras.sun_elevation << "\n"
        << "sky_intensity=" << m_graphicsExtras.sky_intensity << "\n"
        << "sun_shadows=" << m_graphicsExtras.sun_shadows << "\n"
        << "gi=" << m_graphicsExtras.gi << "\n"
        << "gi_strength=" << m_graphicsExtras.gi_strength << "\n"
        << "particle_lights=" << m_graphicsExtras.particle_lights << "\n"
        << "particle_light_strength=" << m_graphicsExtras.particle_light_strength << "\n"
        << "particle_light_radius=" << m_graphicsExtras.particle_light_radius << "\n"
        << "dof_quality=" << m_graphicsExtras.dof_quality << "\n"
        << "dof_strength=" << m_graphicsExtras.dof_strength << "\n"
        << "dof_focus=" << m_graphicsExtras.dof_focus << "\n"
        << "ssr=" << m_graphicsExtras.ssr << "\n"
        << "ssr_strength=" << m_graphicsExtras.ssr_strength << "\n"
        << "texture_scale=" << m_graphicsExtras.texture_scale << "\n"
        << "texture_quality=" << m_graphicsExtras.texture_quality << "\n"
        << "texture_backend=" << m_graphicsExtras.texture_backend << "\n"
        << "hd_budget_mib=" << m_graphicsExtras.hd_budget_mib << "\n"
        << "hd_cache_mib=" << m_graphicsExtras.hd_cache_mib << "\n"
        << "material_enhancement=" << m_graphicsExtras.material_enhancement << "\n"
        << "material_compare=" << m_graphicsExtras.material_compare << "\n"
        << "geometry_refinement=" << m_graphicsExtras.geometry_refinement << "\n"
        << "displacement=" << m_graphicsExtras.displacement << "\n"
        << "pom_quality=" << m_graphicsExtras.pom_quality << "\n"
        << "material_normal_strength=" << m_graphicsExtras.material_normal_strength << "\n"
        << "material_relief=" << m_graphicsExtras.material_relief << "\n"
        << "surface_detail=" << m_graphicsExtras.surface_detail << "\n"
        << "surface_specular=" << m_graphicsExtras.surface_specular << "\n"
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
