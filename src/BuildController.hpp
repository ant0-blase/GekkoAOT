#pragma once

#include <QMap>
#include <QObject>
#include <QProcess>
#include <QString>

struct BuildRequest
{
    QString action;
    QString isoPath;
    QString gameSlug;
    QString backend;
    QString graphicsBackend;
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
    explicit BuildController(QObject* parent = nullptr);
    bool isRunning() const;

public slots:
    void run(const BuildRequest& request);
    void stop();

signals:
    void outputReady(const QString& text);
    void runningChanged(bool running);
    void finished(int exitCode, QProcess::ExitStatus status);

private:
    QString controlProgram() const;
    QProcess m_process;
};
