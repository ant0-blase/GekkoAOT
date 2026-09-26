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
    QString runArgs;
    QString userDir;
    QString windowSystem;
    QString embedWindow;
    QString aspectMode;
    QString resolution;
    bool fullscreen = false;
    int fpsLimit = 120;
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
