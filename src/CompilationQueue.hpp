#pragma once
#include "BuildController.hpp"
#include <QMap>
#include <QPointer>
#include <memory>

struct CompilationJob
{
    enum State { Queued, Running, Cancelling, Completed, Failed, Cancelled };
    quint64 id = 0;
    BuildRequest request;
    QString title, resource, detail, log;
    State state = Queued;
    int progress = 0;
    bool foreground = false;
    bool isActive() const { return state == Running || state == Cancelling; }
    bool isFinished() const { return state >= Completed; }
};

// Compile requests are immutable snapshots of the launcher's settings. Each
// owns a separate controller/process tree; only shared tool preparation is
// serialized by gekkoaotctl. No game is launched by a background compilation.
class CompilationQueue final : public QObject
{
    Q_OBJECT
public:
    explicit CompilationQueue(QObject* parent = nullptr, const QString& program = {});
    ~CompilationQueue() override;
    quint64 enqueue(const BuildRequest& request, const QString& title, const QString& resource = {});
    quint64 trackForeground(BuildController* controller, const BuildRequest& request,
                            const QString& title, const QString& resource = {});
    QList<quint64> ids() const { return m_entries.keys(); }
    const CompilationJob* job(quint64 id) const;
    int activeCount() const;
    int pendingCount() const;
    int maxParallel() const { return m_maxParallel; }
    void setMaxParallel(int count);
    void cancel(quint64 id);
    void cancelAll();
    void retry(quint64 id);
    void clearFinished();

signals:
    void jobAdded(quint64 id);
    void jobChanged(quint64 id);
    void jobRemoved(quint64 id);
    void countsChanged();
    void foregroundCancelRequested();

private:
    struct Entry {
        CompilationJob view;
        QPointer<BuildController> controller;
        QString buffer;
        int secondaryIndex = 0, secondaryTotal = 0;
        QList<QMetaObject::Connection> connections;
    };
    QMap<quint64, std::shared_ptr<Entry>> m_entries;
    QString m_program;
    quint64 m_nextId = 1;
    int m_maxParallel = 2;
    bool m_shuttingDown = false;
    void pump();
    void schedule();
    void bind(const std::shared_ptr<Entry>& entry, BuildController* controller);
    void readOutput(quint64 id, const QString& text);
    void complete(quint64 id, int code, QProcess::ExitStatus status);
    bool resourceBusy(const QString& resource, quint64 except) const;
    static QString resourceFor(const BuildRequest& request, const QString& resource);
};
