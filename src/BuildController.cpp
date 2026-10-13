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
    // Keep this presentation cap independent of the compatibility launch
    // profile so the user's selected value is honored in every GUI mode.
    const int runtimeFpsLimit = request.fpsLimit;
    env.insert("GEKKOAOT_HOST_FPS_LIMIT", QString::number(runtimeFpsLimit));
    // Keep the compatibility boundary policy, but coalesce pure native work.
    // MMIO, HLE, pending interrupts and host epochs still end a superchain.
    // Explicit shell/game overrides, including a zero-cycle budget, win.
    if (!env.contains("GEKKOAOT_NATIVE_OS"))
        env.insert("GEKKOAOT_NATIVE_OS", "0");
    if (!env.contains("GEKKOAOT_SAFE_HOST_BOUNDARIES"))
        env.insert("GEKKOAOT_SAFE_HOST_BOUNDARIES", "1");
    if (!env.contains("GEKKOAOT_SAFE_CHAIN_CYCLES"))
        env.insert("GEKKOAOT_SAFE_CHAIN_CYCLES", "4096");
    // v154: the normal DVD path is a host-native FST/VFS service. Raw DI
    // remains available as a compatibility fallback for direct hardware access.
    if (!env.contains("GEKKOAOT_NATIVE_VFS"))
        env.insert("GEKKOAOT_NATIVE_VFS", "1");
    if (!env.contains("GEKKOAOT_VFS_GC_TIMING"))
        env.insert("GEKKOAOT_VFS_GC_TIMING", "0");
    env.insert("GEKKOAOT_GUI_MATCH_GDB_ACTIVE", matchGdb ? "1" : "0");
    env.insert("GEKKOAOT_PERF_METRICS", "1");
    env.insert("GEKKOAOT_FRAME_INTERPOLATION", "0");
    if (!env.contains("GEKKOAOT_NATIVE_GX_MSAA"))
        env.insert("GEKKOAOT_NATIVE_GX_MSAA", QString::number(request.msaaSamples));
    if (!env.contains("GEKKOAOT_NATIVE_GX_ANISOTROPY"))
        env.insert("GEKKOAOT_NATIVE_GX_ANISOTROPY", QString::number(request.textureAnisotropy));
    env.insert("GEKKOAOT_POST_PROCESSING", request.postProcessing ? "1" : "0");
    env.insert("GEKKOAOT_POSTFX_PRESET", request.postPreset);
    env.insert("GEKKOAOT_POSTFX_SHARPEN", QString::number(request.postSharpen, 'f', 3));
    env.insert("GEKKOAOT_POSTFX_BLOOM", QString::number(request.postBloom, 'f', 3));
    env.insert("GEKKOAOT_POSTFX_VIGNETTE", QString::number(request.postVignette, 'f', 3));
    env.insert("GEKKOAOT_POSTFX_GRAIN", QString::number(request.postGrain, 'f', 3));
    env.insert("GEKKOAOT_POSTFX_TONE", QString::number(request.postTone, 'f', 3));
    env.insert("GEKKOAOT_POSTFX_LOCAL_CONTRAST", QString::number(request.postLocalContrast, 'f', 3));
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

    // v35 Advanced controls are authoritative for the selected launch. NativeOS
    // changes the SDK manifest/static-intercept policy as well as runtime dispatch,
    // so this is a real compile+run selector rather than a cosmetic runtime flag.
    env.insert("GEKKOAOT_NATIVE_OS", request.nativeOs ? "1" : "0");
    env.insert("GEKKOAOT_NATIVE_HW", request.nativeHw ? "1" : "0");
    // NativeHW v2: the umbrella enables every host-native device policy by
    // default. Game-pack environment entries can still set any component to 0
    // before this block to bisect a compatibility regression.
    if (request.nativeHw) {
        const char* nativeHwComponents[] = {
            "GEKKOAOT_NATIVE_HW_SI", "GEKKOAOT_NATIVE_HW_DI",
            "GEKKOAOT_NATIVE_HW_EXI", "GEKKOAOT_NATIVE_HW_VI",
            "GEKKOAOT_NATIVE_HW_AI", "GEKKOAOT_NATIVE_HW_PI",
            "GEKKOAOT_NATIVE_HW_MI", "GEKKOAOT_NATIVE_HW_CP",
            "GEKKOAOT_NATIVE_HW_DSP"
        };
        for (const char* key : nativeHwComponents)
            if (!env.contains(key)) env.insert(key, "1");
    }
    // Apply native defaults after game overrides: explicit values remain
    // authoritative and each path can be disabled independently.
    for (const char* option : {
             "GEKKOAOT_NATIVE_SDK_CLOCK_LOWERING", "GEKKOAOT_NATIVE_HARDWARE_LOWERING",
             "GEKKOAOT_NATIVE_WGPIPE_TRY_LOWERING", "GEKKOAOT_NATIVE_ADDRESS_LOWERING",
             "GEKKOAOT_NATIVE_ADDRESS_BASE", "GEKKOAOT_NATIVE_GX_STREAM_FIFO",
             "GEKKOAOT_NATIVE_FLAT_VMEM", "GEKKOAOT_NATIVE_LOCKED_CACHE", "GEKKOAOT_NATIVE_GX_SDK_PACKETS",
             "GEKKOAOT_NATIVE_GX_NORMAL_MATRIX", "GEKKOAOT_NATIVE_DVD_FST",
             "GEKKOAOT_NATIVE_PAD", "GEKKOAOT_SDK_SERVICE_CORPUS", "GEKKOAOT_GX_ORDERED_PE_IRQ",
             "GEKKOAOT_GX_DIRECT_BATCH_SLOT", "GEKKOAOT_NATIVE_GX_DIRECT_STATE",
             "GEKKOAOT_SECONDARY_DIRECT_DISPATCH", "GEKKOAOT_SECONDARY_PAGE_DISPATCH",
             "GEKKOAOT_FP_REGION_COALESCE", "GEKKOAOT_SECONDARY_INSTRUCTION_DISPATCH",
             "GEKKOAOT_SECONDARY_BOUNDARY_HOIST",
             "GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS", "GEKKOAOT_NATIVE_GX_BIND_GROUP_CACHE",
             "GEKKOAOT_NATIVE_GX_SNAPSHOT_ATLAS", "GEKKOAOT_NATIVE_GX_ARRAY_TILES",
             "GEKKOAOT_NATIVE_GX_ARRAY_SNAPSHOTS", "GEKKOAOT_NATIVE_GX_SEMANTIC_BATCH"})
        if (!env.contains(option)) env.insert(option, "1");
    // v307 exact index decode caching costs more than rescanning Harry's tiny
    // draws in gameplay. Keep the tested option available, disabled by default.
    if (!env.contains("GEKKOAOT_NATIVE_GX_INDEX_RANGE_CACHE"))
        env.insert("GEKKOAOT_NATIVE_GX_INDEX_RANGE_CACHE", "0");
    if (!env.contains("GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN"))
        env.insert("GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN", "1");
    if (!env.contains("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE"))
        env.insert("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE", "1");
    // v313: recognized SDK array bindings reach the renderer as typed host
    // commands in the existing batch. Custom/backlogged paths retain decoding.
    if (!env.contains("GEKKOAOT_NATIVE_GX_TYPED_ARRAYS"))
        env.insert("GEKKOAOT_NATIVE_GX_TYPED_ARRAYS", "1");
    if (!env.contains("GEKKOAOT_NATIVE_GX_DIRECT_MATRICES"))
        env.insert("GEKKOAOT_NATIVE_GX_DIRECT_MATRICES", "1");
    // v318: exact read-only waits use host events; short indexed draws upload
    // only their referenced elements. Each route retains its own off switch.
    for (const QString option : {"GEKKOAOT_NATIVE_READ_POLL",
                                 "GEKKOAOT_NATIVE_COUNTER_POLL",
                                 "GEKKOAOT_NATIVE_GX_VERTEX_GATHER"})
        if (!env.contains(option)) env.insert(option, "1");
    // Resolved-state fusion is independently available but added overhead in
    // Harry's Burrow profile; do not pay its CPU shadows without opting in.
    if (!env.contains("GEKKOAOT_NATIVE_GX_RESOLVED_BATCH"))
        env.insert("GEKKOAOT_NATIVE_GX_RESOLVED_BATCH", "0");
    if (!env.contains("GEKKOAOT_NATIVE_OS_EAGER_FPU"))
        env.insert("GEKKOAOT_NATIVE_OS_EAGER_FPU", "0");
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_OS_FPU_V309=1 eager=%1 ownership=sdk-lazy-fp-transfer\n")
        .arg(env.value("GEKKOAOT_NATIVE_OS_EAGER_FPU")));
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_GX_V311=1 sdk_arrays=%1 attribute_plan=%2\n")
        .arg(env.value("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN")));
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_GX_V313=1 typed_arrays=%1 submission=native-command-batch\n")
        .arg(env.value("GEKKOAOT_NATIVE_GX_TYPED_ARRAYS")));
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_GX_V317=1 direct_matrices=%1 renderer=matrix-state\n")
        .arg(env.value("GEKKOAOT_NATIVE_GX_DIRECT_MATRICES")));
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_V318=1 ram_wait=%1 counter_wait=%2 gathered_vertices=%3\n")
        .arg(env.value("GEKKOAOT_NATIVE_READ_POLL"))
        .arg(env.value("GEKKOAOT_NATIVE_COUNTER_POLL"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_VERTEX_GATHER")));
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_SDK_V308=1 flat_vmem=%1 gx_packets=%2 normal_matrix=%3 dvd_fst=%4 pad=%5 service_corpus=%6 ordered_pe=%7 locked_cache=%8 index_cache=%9\n")
        .arg(env.value("GEKKOAOT_NATIVE_FLAT_VMEM"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_SDK_PACKETS"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_NORMAL_MATRIX"))
        .arg(env.value("GEKKOAOT_NATIVE_DVD_FST"))
        .arg(env.value("GEKKOAOT_NATIVE_PAD"))
        .arg(env.value("GEKKOAOT_SDK_SERVICE_CORPUS"))
        .arg(env.value("GEKKOAOT_GX_ORDERED_PE_IRQ"))
        .arg(env.value("GEKKOAOT_NATIVE_LOCKED_CACHE"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_INDEX_RANGE_CACHE")));
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_GX_V303 atlas=%1 array_tiles=%2 small_arrays=%3 semantic_batch=%4 resolved_batch=%5 fallback=enabled\n")
        .arg(env.value("GEKKOAOT_NATIVE_GX_SNAPSHOT_ATLAS"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_ARRAY_TILES"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_ARRAY_SNAPSHOTS"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_SEMANTIC_BATCH"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_RESOLVED_BATCH")));
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_POLICY_V291=1 chain_cycles=%1 sdk_clock=%2 hardware=%3 wgpipe=%4 address=%5 gx_stream=%6 gx_state=%7 batch_slot=%8 secondary_direct=%9 secondary_pages=%10\n")
        .arg(env.value("GEKKOAOT_SAFE_CHAIN_CYCLES"))
        .arg(env.value("GEKKOAOT_NATIVE_SDK_CLOCK_LOWERING"))
        .arg(env.value("GEKKOAOT_NATIVE_HARDWARE_LOWERING"))
        .arg(env.value("GEKKOAOT_NATIVE_WGPIPE_TRY_LOWERING"))
        .arg(env.value("GEKKOAOT_NATIVE_ADDRESS_LOWERING"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_STREAM_FIFO"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_DIRECT_STATE"))
        .arg(env.value("GEKKOAOT_GX_DIRECT_BATCH_SLOT"))
        .arg(env.value("GEKKOAOT_SECONDARY_DIRECT_DISPATCH"))
        .arg(env.value("GEKKOAOT_SECONDARY_PAGE_DISPATCH")));
    emit outputReady(QString("GEKKOAOT_GUI_NATIVE_POLICY_V298=1 fp_regions=%1 instruction_routes=%2 boundary_hoist=%3 exception_vector=%4 compact_arrays=%5 gpu_bind_cache=%6 address_publication=%7\n")
        .arg(env.value("GEKKOAOT_FP_REGION_COALESCE"))
        .arg(env.value("GEKKOAOT_SECONDARY_INSTRUCTION_DISPATCH"))
        .arg(env.value("GEKKOAOT_SECONDARY_BOUNDARY_HOIST"))
        .arg(env.value("GEKKOAOT_NATIVE_EXCEPTION_VECTOR", "0"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS", "0"))
        .arg(env.value("GEKKOAOT_NATIVE_GX_BIND_GROUP_CACHE", "0"))
        .arg(env.value("GEKKOAOT_NATIVE_ADDRESS_PUBLICATION", "0")));
    const bool parallelWorkers = request.runtimeWorkers.compare("serial", Qt::CaseInsensitive) != 0;
    env.insert("GEKKOAOT_RUNTIME_PARALLEL", parallelWorkers ? "1" : "0");
    env.insert("GEKKOAOT_RUNTIME_WORKERS", parallelWorkers ? "parallel" : "serial");
    emit outputReady(QString("GEKKOAOT_GUI_RUNTIME_POLICY_V36=1 native_os=%1 native_hw=%5 workers=%2 cpu_order=single gx=%4 dsp=cpu-ordered host-audio=async io=%3\n")
                         .arg(request.nativeOs ? 1 : 0)
                         .arg(parallelWorkers ? "parallel" : "serial")
                         .arg(parallelWorkers ? "async" : "inline")
                         .arg(parallelWorkers ? "parser-worker+render-worker" : "inline-parser+render-worker")
                         .arg(request.nativeHw ? 1 : 0));

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
