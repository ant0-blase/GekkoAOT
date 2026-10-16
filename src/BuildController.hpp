#pragma once
#include "../runtime/gx/graphics_settings.h"

#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>

struct BuildRequest
{
    QString action;
    QString isoPath;
    QString gameSlug;
    QString backend;
    QString graphicsBackend;
    GekkoAOT::GX::GraphicsSettings graphics;
    QString profile;
    QString toolchain;
    QString llvmDir;
    QString nativeAbi;
    bool nativeOs = true;
    bool nativeHw = true;
    QString runtimeWorkers = "parallel";
    QString runArgs;
    QString userDir;
    QString windowSystem;
    QString embedWindow;
    bool persistentShell = false;
    QString aspectMode;
    QString resolution;
    bool fullscreen = false;
    bool frameInterpolation = false;
    int fpsLimit = 120;
    int msaaSamples = 1;
    int textureAnisotropy = 16;
    bool postProcessing = true;
    QString postPreset = "balanced";
    double postSharpen = 0.28;
    double postBloom = 0.10;
    double postVignette = 0.06;
    double postGrain = 0.0;
    double postTone = 0.15;
    double postLocalContrast = 0.08;
    QString uiScale;
    QString inputConfig;
    QMap<QString, QString> gameEnvironment;
    int jobs = 1;
};

class BuildController final : public QObject
{
    Q_OBJECT
public:
    explicit BuildController(QObject* parent = nullptr, const QString& program = {});
    ~BuildController() override;
    bool isRunning() const;

public slots:
    void run(const BuildRequest& request);
    void stop();

signals:
    void outputReady(const QString& text);
    void runningChanged(bool running);
    void presentationEmbedded(bool embedded);
    void cancellationRequested();
    void finished(int exitCode, QProcess::ExitStatus status);

private:
    QString controlProgram() const;
    void terminateTree(bool force);
    void complete(int code, QProcess::ExitStatus status);
    QProcess m_process;
    QTimer m_stopTimer;
    QString m_program;
    qint64 m_processGroup = 0;
    bool m_stopping = false;
    bool m_finished = true;
#ifdef Q_OS_WIN
    void* m_job = nullptr;
#endif
};
