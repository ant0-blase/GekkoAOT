#include "CompilationQueue.hpp"
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>

CompilationQueue::CompilationQueue(QObject* parent, const QString& program) : QObject(parent), m_program(program) {}
CompilationQueue::~CompilationQueue()
{
    m_shuttingDown = true;
    for (const auto& entry : m_entries) {
        for (const auto& connection : entry->connections) disconnect(connection);
        if (entry->controller && !entry->view.foreground) delete entry->controller.data();
    }
}
QString CompilationQueue::resourceFor(const BuildRequest& request, const QString& resource)
{
    if (!resource.isEmpty()) return resource;
    QFileInfo file(request.isoPath);
    const QString canonical = file.canonicalFilePath();
    return canonical.isEmpty() ? file.absoluteFilePath() : canonical;
}
const CompilationJob* CompilationQueue::job(quint64 id) const
{
    const auto entry = m_entries.value(id);
    return entry ? &entry->view : nullptr;
}
int CompilationQueue::activeCount() const
{
    int count = 0;
    for (const auto& entry : m_entries) count += entry->view.isActive();
    return count;
}
int CompilationQueue::pendingCount() const
{
    int count = 0;
    for (const auto& entry : m_entries) count += entry->view.state == CompilationJob::Queued;
    return count;
}
void CompilationQueue::setMaxParallel(int count)
{
    m_maxParallel = std::clamp(count, 1, 8);
    emit countsChanged();
    schedule();
}
quint64 CompilationQueue::enqueue(const BuildRequest& source, const QString& title, const QString& resource)
{
    const QString key = resourceFor(source, resource);
    for (const auto& entry : m_entries)
        if (entry->view.resource == key && !entry->view.isFinished()) return entry->view.id;
    auto entry = std::make_shared<Entry>();
    entry->view.id = m_nextId++;
    entry->view.request = source;
    entry->view.request.action = "compile";
    entry->view.request.embedWindow.clear();
    entry->view.title = title;
    entry->view.resource = key;
    entry->view.detail = "Waiting in queue";
    m_entries.insert(entry->view.id, entry);
    emit jobAdded(entry->view.id);
    emit countsChanged();
    schedule();
    return entry->view.id;
}
quint64 CompilationQueue::trackForeground(BuildController* controller, const BuildRequest& request,
                                          const QString& title, const QString& resource)
{
    auto entry = std::make_shared<Entry>();
    entry->view.id = m_nextId++;
    entry->view.request = request;
    entry->view.title = title;
    entry->view.resource = resourceFor(request, resource);
    entry->view.foreground = true;
    entry->view.state = CompilationJob::Running;
    entry->view.detail = "Compile and play";
    m_entries.insert(entry->view.id, entry);
    bind(entry, controller);
    emit jobAdded(entry->view.id);
    emit countsChanged();
    return entry->view.id;
}
void CompilationQueue::schedule()
{
    if (!m_shuttingDown) QTimer::singleShot(0, this, &CompilationQueue::pump);
}
bool CompilationQueue::resourceBusy(const QString& resource, quint64 except) const
{
    for (const auto& entry : m_entries) {
        if (entry->view.id == except || entry->view.resource != resource) continue;
        if (entry->view.isActive() || (entry->view.foreground && entry->controller && entry->controller->isRunning()))
            return true;
    }
    return false;
}
void CompilationQueue::pump()
{
    if (m_shuttingDown) return;
    // Snapshot ids: synchronous failure signals must not invalidate iteration.
    for (quint64 id : ids()) {
        if (activeCount() >= m_maxParallel) break;
        const auto entry = m_entries.value(id);
        if (!entry || entry->view.state != CompilationJob::Queued) continue;
        if (resourceBusy(entry->view.resource, id)) {
            entry->view.detail = "Waiting for this game's current task";
            emit jobChanged(id);
            continue;
        }
        entry->view.state = CompilationJob::Running;
        entry->view.detail = "Preparing native toolchain…";
        auto* controller = new BuildController(this, m_program);
        bind(entry, controller);
        auto request = entry->view.request;
        // Share the configured compiler thread budget, rather than multiplying
        // it by the number of simultaneous games. Optimization flags stay intact.
        request.jobs = qMax(1, request.jobs / m_maxParallel);
        emit jobChanged(id);
        emit countsChanged();
        controller->run(request);
    }
}
void CompilationQueue::bind(const std::shared_ptr<Entry>& entry, BuildController* controller)
{
    const auto id = entry->view.id;
    entry->controller = controller;
    entry->connections << connect(controller, &BuildController::outputReady, this,
        [this, id](const QString& text) { readOutput(id, text); });
    entry->connections << connect(controller, &BuildController::finished, this,
        [this, id](int code, QProcess::ExitStatus status) { complete(id, code, status); });
    entry->connections << connect(controller, &BuildController::cancellationRequested, this, [this, id] {
        const auto current = m_entries.value(id);
        if (!current || current->view.isFinished()) return;
        current->view.state = CompilationJob::Cancelling;
        current->view.detail = "Cancelling…";
        emit jobChanged(id);
        emit countsChanged();
    });
}
void CompilationQueue::readOutput(quint64 id, const QString& text)
{
    const auto entry = m_entries.value(id);
    if (!entry || entry->view.isFinished()) return;
    auto& job = entry->view;
    job.log += text;
    if (job.log.size() > 131072) job.log = job.log.right(131072);
    entry->buffer += text;
    static const QRegularExpression progress(R"(GEKKOAOT_PROGRESS=(\d+)\|(.+))");
    static const QRegularExpression ninja(R"(^\[(\d+)/(\d+)\]\s*(.*))");
    static const QRegularExpression secondary(
        R"re(GEKKOAOT_SECONDARY_ITEM=1\s+kind=(\w+)\s+index=(\d+)\s+total=(\d+)\s+source="([^"]+)")re");
    while (true) {
        const auto end = entry->buffer.indexOf('\n');
        if (end < 0) break;
        const QString line = entry->buffer.left(end).trimmed();
        entry->buffer.remove(0, end + 1);
        if (job.state == CompilationJob::Cancelling) continue;
        const auto marker = progress.match(line);
        const auto step = ninja.match(line);
        const auto module = secondary.match(line);
        if (marker.hasMatch()) {
            job.progress = qMax(job.progress, qBound(0, marker.captured(1).toInt(), 99));
            job.detail = marker.captured(2);
        } else if (line.startsWith("GEKKOAOT_PIPELINE_LOCK_WAIT=")) {
            job.detail = "Waiting for shared toolchain or game cache";
        } else if (step.hasMatch()) {
            job.detail = QString("Compiling AOT blocks · %1/%2").arg(step.captured(1), step.captured(2));
            if (entry->secondaryTotal)
                job.progress = qMax(job.progress, qBound(93, 93 + int((qint64(entry->secondaryIndex - 1) *
                    step.captured(2).toLongLong() + step.captured(1).toLongLong()) * 6 /
                    qMax<qint64>(1, qint64(entry->secondaryTotal) * step.captured(2).toLongLong())), 99));
            else if (line.contains("LLVM object"))
                job.progress = qMax(job.progress, qBound(50, 50 + step.captured(1).toInt() * 40 /
                    qMax(1, step.captured(2).toInt()), 90));
        } else if (module.hasMatch()) {
            entry->secondaryIndex = module.captured(2).toInt();
            entry->secondaryTotal = qMax(1, module.captured(3).toInt());
            job.detail = QString("%1 %2/%3 · %4").arg(module.captured(1), module.captured(2),
                module.captured(3), QFileInfo(module.captured(4)).fileName());
        } else if (line.startsWith("GEKKOAOT_SUBSTAGE=")) {
            job.detail = line.mid(18).replace('-', ' ');
        }
        if (job.foreground && line == "GEKKOAOT_STAGE=runtime") {
            job.state = CompilationJob::Completed;
            job.progress = 100;
            job.detail = "Ready · game launched";
            emit countsChanged();
            schedule();
            break;
        }
    }
    if (entry->buffer.size() > 16384) entry->buffer = entry->buffer.right(16384);
    emit jobChanged(id);
}
void CompilationQueue::complete(quint64 id, int code, QProcess::ExitStatus status)
{
    const auto entry = m_entries.value(id);
    if (!entry) return;
    auto& job = entry->view;
    if (!job.isFinished()) {
        if (job.state == CompilationJob::Cancelling) {
            job.state = CompilationJob::Cancelled;
            job.detail = "Cancelled";
        } else if (code == 0 && status == QProcess::NormalExit) {
            job.state = CompilationJob::Completed;
            job.progress = 100;
            job.detail = "Ready to play";
        } else {
            job.state = CompilationJob::Failed;
            job.detail = QString("Failed · exit %1 · see log").arg(code);
        }
    }
    for (const auto& connection : entry->connections) disconnect(connection);
    entry->connections.clear();
    if (!job.foreground && entry->controller) entry->controller->deleteLater();
    entry->controller.clear();
    emit jobChanged(id);
    emit countsChanged();
    schedule();
}
void CompilationQueue::cancel(quint64 id)
{
    const auto entry = m_entries.value(id);
    if (!entry || entry->view.isFinished() || entry->view.state == CompilationJob::Cancelling) return;
    if (entry->view.state == CompilationJob::Queued) {
        entry->view.state = CompilationJob::Cancelled;
        entry->view.detail = "Cancelled";
    } else {
        entry->view.state = CompilationJob::Cancelling;
        entry->view.detail = "Cancelling…";
        if (entry->view.foreground) emit foregroundCancelRequested();
        if (entry->controller) entry->controller->stop();
    }
    emit jobChanged(id);
    emit countsChanged();
    schedule();
}
void CompilationQueue::cancelAll()
{
    // Cancel queued entries first so no completion can start another build.
    const auto list = ids();
    for (auto id : list) if (job(id)->state == CompilationJob::Queued) cancel(id);
    for (auto id : list) cancel(id);
}
void CompilationQueue::retry(quint64 id)
{
    const auto entry = m_entries.value(id);
    if (!entry || !entry->view.isFinished()) return;
    enqueue(entry->view.request, entry->view.title, entry->view.resource);
}
void CompilationQueue::clearFinished()
{
    for (auto id : ids()) {
        const auto entry = m_entries.value(id);
        if (!entry->view.isFinished()) continue;
        // A foreground record also guards its running game's cache. Keep that
        // guard until the game exits, even if its compilation has completed.
        if (entry->view.foreground && entry->controller && entry->controller->isRunning()) continue;
        m_entries.remove(id);
        emit jobRemoved(id);
    }
    emit countsChanged();
}
