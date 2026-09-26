#include "BuildController.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

#ifndef GEKKOAOT_SOURCE_DIR
#define GEKKOAOT_SOURCE_DIR "."
#endif
#ifndef GEKKOAOT_VERSION
#define GEKKOAOT_VERSION "dev"
#endif

BuildController::BuildController(QObject* parent) : QObject(parent)
{
    m_process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_UNIX
    m_process.setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray bytes = m_process.readAllStandardOutput();
        if (!bytes.isEmpty())
            emit outputReady(QString::fromLocal8Bit(bytes));
    });
    connect(&m_process, &QProcess::started, this, [this] { emit runningChanged(true); });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                const QByteArray tail = m_process.readAllStandardOutput();
                if (!tail.isEmpty())
                    emit outputReady(QString::fromLocal8Bit(tail));
                emit runningChanged(false);
                emit finished(code, status);
            });
}

bool BuildController::isRunning() const { return m_process.state() != QProcess::NotRunning; }

QString BuildController::controlProgram() const
{
    const QDir appDir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QString local = appDir.filePath("gekkoaotctl.exe");
#else
    const QString local = appDir.filePath("gekkoaotctl");
#endif
    if (QFileInfo::exists(local))
        return local;

    const QDir source(QString::fromUtf8(GEKKOAOT_SOURCE_DIR));
#ifdef Q_OS_WIN
    const QString sourceBuild = source.filePath("build/bin/gekkoaotctl.exe");
#else
    const QString sourceBuild = source.filePath("build/bin/gekkoaotctl");
#endif
    return sourceBuild;
}

void BuildController::run(const BuildRequest& request)
{
    if (isRunning()) {
        emit outputReady("Another GekkoAOT task is already running.\n");
        return;
    }

    const QString controller = controlProgram();
    if (!QFileInfo::exists(controller)) {
        emit outputReady(QString("gekkoaotctl was not built beside the GUI. Rebuild GekkoAOT v%1.\n")
                             .arg(QStringLiteral(GEKKOAOT_VERSION)));
        emit finished(2, QProcess::NormalExit);
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    // v79 compatibility launch profile. The GDB runner proved that several
    // retail titles need host-visible AOT boundaries more frequently than the
    // normal 262144-cycle super-chain allows. Reproduce the known-good runtime
    // semantics without attaching GDB. Set GEKKOAOT_GUI_MATCH_GDB=0 to opt
    // back into the embedded/high-throughput profile for performance testing.
    const QString matchText = env.value("GEKKOAOT_GUI_MATCH_GDB", "1").trimmed().toLower();
    const bool matchGdb = !(matchText == "0" || matchText == "false" ||
                            matchText == "off" || matchText == "no");

    env.insert("GEKKOAOT_ISO", request.isoPath);
    env.insert("GEKKOAOT_GAME", request.gameSlug);
    env.insert("GEKKOAOT_BACKEND", request.backend);
    env.insert("GEKKOAOT_GRAPHICS_BACKEND", request.graphicsBackend);
    env.insert("GEKKOAOT_PROFILE", request.profile);
    env.insert("GEKKOAOT_TOOLCHAIN", request.toolchain);
    env.insert("GEKKOAOT_LLVM_DIR", request.llvmDir);
    env.insert("GEKKOAOT_NATIVE_ABI", request.nativeAbi);
    env.insert("GEKKOAOT_JOBS", QString::number(request.jobs));
    env.insert("GEKKOAOT_RUN_ARGS", request.runArgs);
    env.insert("GEKKOAOT_USER_DIR", request.userDir);
    env.insert("GEKKOAOT_WINDOW_SYSTEM", request.windowSystem);
    const QString runtimeEmbedWindow = matchGdb ? QString() : request.embedWindow;
    env.insert("GEKKOAOT_EMBED_WINDOW", runtimeEmbedWindow);
    env.insert("GEKKOAOT_NATIVE_GX_SINGLE_WINDOW", runtimeEmbedWindow.isEmpty() ? "0" : "1");
    // Compatibility alias used by the established v42 Aurora X11 path.
    if (runtimeEmbedWindow.isEmpty())
        env.remove("GEKKOAOT_NATIVE_GX_X11_WINDOW");
    else
        env.insert("GEKKOAOT_NATIVE_GX_X11_WINDOW", runtimeEmbedWindow);
#ifdef Q_OS_LINUX
    // Only force SDL/X11 when we are actually embedding the Aurora surface.
    // The GDB-match profile intentionally keeps Aurora standalone so the GUI
    // and debugger exercise the same SDL/WebGPU presentation path.
    if (!matchGdb && request.windowSystem.compare("x11", Qt::CaseInsensitive) == 0 &&
        !runtimeEmbedWindow.isEmpty())
        env.insert("SDL_VIDEODRIVER", "x11");
#endif
    env.insert("GEKKOAOT_ASPECT_MODE", request.aspectMode);
    env.insert("GEKKOAOT_RESOLUTION", request.resolution);
    env.insert("GEKKOAOT_FULLSCREEN", request.fullscreen ? "1" : "0");
    // v47: the PC-style FPS setting belongs exclusively to host presentation.
    // AOT execution stays uncapped; VI/TB/DEC/DSP/AI use their own realtime
    // hardware clock and are never multiplied by the selected render cap.
    // GDB does not inherit the GUI FPS slider and therefore uses the runtime
    // default of 120 Hz. Match that known-good scheduling profile in v79.
    const int runtimeFpsLimit = matchGdb ? 120 : request.fpsLimit;
    env.insert("GEKKOAOT_HOST_FPS_LIMIT", QString::number(runtimeFpsLimit));
    // v153 correctness defaults: keep the SDK OS in guest PPC->AOT and force a
    // HostRuntime boundary after every generated dispatch.  Explicit shell/game
    // overrides still win, so NativeOS / chaining remain available for testing.
    if (!env.contains("GEKKOAOT_NATIVE_OS"))
        env.insert("GEKKOAOT_NATIVE_OS", "0");
    if (!env.contains("GEKKOAOT_SAFE_HOST_BOUNDARIES"))
        env.insert("GEKKOAOT_SAFE_HOST_BOUNDARIES", "1");
    if (!env.contains("GEKKOAOT_SAFE_CHAIN_CYCLES"))
        env.insert("GEKKOAOT_SAFE_CHAIN_CYCLES", "0");
    // v154: the normal DVD path is a host-native FST/VFS service. Raw DI
    // remains available as a compatibility fallback for direct hardware access.
    if (!env.contains("GEKKOAOT_NATIVE_VFS"))
        env.insert("GEKKOAOT_NATIVE_VFS", "1");
    if (!env.contains("GEKKOAOT_VFS_GC_TIMING"))
        env.insert("GEKKOAOT_VFS_GC_TIMING", "0");
    env.insert("GEKKOAOT_GUI_MATCH_GDB_ACTIVE", matchGdb ? "1" : "0");
    env.insert("GEKKOAOT_PERF_METRICS", "1");
    if (!env.contains("GEKKOAOT_FRAME_INTERPOLATION"))
        env.insert("GEKKOAOT_FRAME_INTERPOLATION", "1");
    env.remove("GEKKOAOT_VI_WAIT_TARGET_HZ");
    env.remove("GEKKOAOT_GUEST_REALTIME_PACING");
    // v110 correctness default: GXCopyDisp submits the producer XFB, while VI
    // owns scanout timing.  A gameEnvironment override may still opt into
    // copy-driven host presentation for profiling, without changing guest VI.
    env.insert("GEKKOAOT_NATIVE_GX_PRESENT_ON_COPY", "0");
    env.insert("GEKKOAOT_NATIVE_GX_PRESENT_FALLBACK_VI", "1");
    env.insert("GEKKOAOT_UI_SCALE", request.uiScale);
    env.insert("GEKKOAOT_INPUT_CONFIG", request.inputConfig);
    for (auto it = request.gameEnvironment.cbegin(); it != request.gameEnvironment.cend(); ++it)
        env.insert(it.key(), it.value());

    if (matchGdb) {
        emit outputReady("GEKKOAOT_GUI_MATCH_GDB_V140=1 host_fps=120 native_chain=bounded gx_window=standalone diagnostics=off spin-yield=off\n");
    } else {
        emit outputReady(QString("GEKKOAOT_GUI_MATCH_GDB_V140=0 host_fps=%1 native_chain=fast gx_window=embedded spin-yield=off\n")
                             .arg(request.fpsLimit));
    }

    m_process.setProcessEnvironment(env);
    m_process.setWorkingDirectory(QFileInfo(controller).absolutePath());
    m_process.setProgram(controller);
    m_process.setArguments({request.action});
    emit outputReady(QString("$ %1 %2\n").arg(controller, request.action));
    m_process.start();
}

void BuildController::stop()
{
    if (!isRunning()) return;
    emit outputReady("\nStopping current task and native runtime...\n");
#ifdef Q_OS_UNIX
    const qint64 pid = m_process.processId();
    if (pid > 0) ::kill(-static_cast<pid_t>(pid), SIGTERM); else m_process.terminate();
    if (!m_process.waitForFinished(1800)) {
        if (pid > 0) ::kill(-static_cast<pid_t>(pid), SIGKILL); else m_process.kill();
        m_process.waitForFinished(1500);
    }
#else
    m_process.terminate();
    if (!m_process.waitForFinished(1800)) { m_process.kill(); m_process.waitForFinished(1500); }
#endif
}
