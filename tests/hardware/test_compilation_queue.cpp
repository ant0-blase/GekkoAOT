#include "../../src/CompilationQueue.hpp"
#include "../../src/CompilationSidebar.hpp"
#include "../../src/ShellNavigation.hpp"
#include "../../src/ShellStyle.hpp"
#include <QDialog>
#include <QVBoxLayout>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#ifdef Q_OS_UNIX
#include <csignal>
#endif
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
static QByteArray read(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{}; }
static void touch(const QString& path) { QFile f(path); CHECK(f.open(QIODevice::WriteOnly)); f.write("1"); }
static void wait(const std::function<bool()>& done, int timeout = 6500) {
    QElapsedTimer clock; clock.start();
    while (!done() && clock.elapsed() < timeout) { QCoreApplication::processEvents(); QThread::msleep(5); }
    CHECK(done());
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    CHECK(argc >= 2);
    QTemporaryDir temp;
    CHECK(temp.isValid());
    auto request = [&](const QString& slug) {
        BuildRequest r;
        r.action = "compile"; r.isoPath = temp.path() + "/" + slug + ".rvz";
        r.gameSlug = slug; r.userDir = temp.path() + "/" + slug; r.jobs = 8;
        r.backend = "llvm"; r.profile = "native"; r.nativeHw = true;
        return r;
    };
    CompilationQueue queue(nullptr, argv[1]);
    QDialog panel;panel.resize(540,900);
    auto* layout=new QVBoxLayout(&panel);
    CompilationSidebar sidebar(&queue);layout->addWidget(&sidebar);
    presentShellDialog(panel,"Compilations",true);panel.show();
    const auto a = queue.enqueue(request("resistant"), "Harry Potter · compiling");
    const auto b = queue.enqueue(request("hold"), "Luigi's Mansion · compiling");
    const auto c = queue.enqueue(request("success"), "GoldenEye · queued");
    const auto d = queue.enqueue(request("cancel-queued"), "Call of Duty · queued");
    const auto f = queue.enqueue(request("fail"), "Failure and retry");
    CHECK(queue.enqueue(request("hold"), "duplicate") == b);
    queue.cancel(d);
    wait([&] { return read(temp.path() + "/hold/started").size() && queue.job(a)->progress >= 47 && queue.job(b)->progress >= 47; });
    CHECK(queue.activeCount() == 2 && queue.job(c)->state == CompilationJob::Queued);
    const auto started = read(temp.path() + "/hold/started");
    CHECK(started.contains("\n4\nllvm\n1\nnative\ncompile"));
    CHECK(!QFile::exists(temp.path() + "/cancel-queued/started"));
    if (argc > 2) sidebar.grab().save(argv[2]);
    QPushButton* cancel = nullptr;
    for (auto* card : sidebar.findChildren<QWidget*>("compilationCard"))
        if (card->property("jobId").toULongLong() == a) cancel = card->findChild<QPushButton*>("cancelCompilation");
    CHECK(cancel && cancel->isEnabled());
    // Follow the actual rows, including the right-hand numeric control and
    // Cancel buttons, rather than Qt's creation/tab order.
    panel.findChild<QPushButton*>("shellDialogBack")->setFocus();
    bool reachedCancel=false,reachedFooter=false;
    for(int step=0;step<20;++step) {
        auto* focus=QApplication::focusWidget();
        reachedCancel|=focus==cancel;
        reachedFooter|=focus && (focus->objectName()=="cancelAllCompilations" || focus->objectName()=="clearCompilations");
        shellNavigate(&panel,Qt::Key_Down);QCoreApplication::processEvents();
    }
    CHECK(reachedCancel && reachedFooter);
    if(QApplication::focusWidget()->objectName()=="clearCompilations")shellNavigate(&panel,Qt::Key_Left);
    CHECK(QApplication::focusWidget()->objectName()=="cancelAllCompilations");
    sidebar.findChild<QPushButton*>("compilationDetails")->click();QCoreApplication::processEvents();
    auto* logPanel=panel.findChild<QDialog*>("compilationLogPanel");
    CHECK(logPanel && logPanel->isVisible() && !logPanel->isWindow() && logPanel->parentWidget()==&panel);
    CHECK(logPanel->geometry()==panel.rect());logPanel->close();QCoreApplication::processEvents();
    QElapsedTimer elapsed; elapsed.start(); cancel->click();
    CHECK(elapsed.elapsed() < 150); // Cancelling must not block the event loop.
    CHECK(queue.job(a)->state == CompilationJob::Cancelling);
    wait([&] { return queue.job(a)->state == CompilationJob::Cancelled && queue.job(c)->isFinished() && queue.job(f)->isFinished(); });
    CHECK(queue.job(c)->state == CompilationJob::Completed && queue.job(c)->progress == 100);
    QString played;
    sidebar.playRequested=[&](const QString& path){played=path;};
    for(auto* card:sidebar.findChildren<QWidget*>("compilationCard"))if(card->property("jobId").toULongLong()==c) {
        auto* play=card->findChild<QPushButton*>("playCompilation");CHECK(play && play->isVisible());play->click();
        CHECK(played==queue.job(c)->request.isoPath);
    }
    CHECK(queue.job(f)->state == CompilationJob::Failed && queue.job(f)->log.contains("deliberate failure"));
    CHECK(queue.job(b)->state == CompilationJob::Running);
#ifdef Q_OS_UNIX
    const auto root = read(temp.path() + "/resistant/started").split('\n')[0].toLongLong();
    CHECK(root > 0 && kill(pid_t(root), 0) == -1);
    const auto child = read(temp.path() + "/resistant/child").toLongLong();
    CHECK(child > 0);
    wait([&] {
        const auto stat = read(QString("/proc/%1/stat").arg(child));
        return kill(pid_t(child), 0) == -1 || stat.contains(") Z");
    });
#endif
    queue.retry(f);
    const auto retried = queue.ids().last();
    CHECK(retried != f);
    wait([&] { return queue.job(retried)->state == CompilationJob::Completed; });
    touch(temp.path() + "/hold/release");
    wait([&] { return queue.activeCount() == 0 && queue.pendingCount() == 0; });
    queue.clearFinished();
    CHECK(queue.ids().isEmpty());
    // A new limit takes effect on the next start, preserving active workers.
    queue.setMaxParallel(1);
    const auto one = queue.enqueue(request("hold-one"), "One");
    const auto two = queue.enqueue(request("success-two"), "Two");
    wait([&] { return QFile::exists(temp.path() + "/hold-one/started"); });
    CHECK(queue.job(two)->state == CompilationJob::Queued);
    CHECK(read(temp.path() + "/hold-one/started").contains("\n8\nllvm"));
    queue.setMaxParallel(2);
    wait([&] { return queue.job(two)->state == CompilationJob::Completed; });
    queue.cancelAll(); wait([&] { return queue.job(one)->state == CompilationJob::Cancelled; });
    // An Open Game build is also cancellable in the sidebar. Once it reaches
    // runtime, Cancel all must leave the running game alone.
    BuildController player(nullptr, argv[1]);
    auto play = request("runtime"); play.action = "run";
    const auto foreground = queue.trackForeground(&player, play, "Foreground", "game-id");
    player.run(play);
    wait([&] { return QFile::exists(play.userDir + "/started"); });
    touch(play.userDir + "/release");
    wait([&] { return queue.job(foreground)->state == CompilationJob::Completed; });
    CHECK(player.isRunning());
    const auto duplicate = queue.enqueue(play, "Same game", "game-id");
    QCoreApplication::processEvents();
    CHECK(queue.job(duplicate)->state == CompilationJob::Queued);
    queue.cancelAll();
    CHECK(player.isRunning() && queue.job(duplicate)->state == CompilationJob::Cancelled);
    touch(play.userDir + "/close");
    wait([&] { return !player.isRunning(); });
    auto cancelledPlay = request("runtime-cancelled"); cancelledPlay.action = "run";
    const auto stopped = queue.trackForeground(&player, cancelledPlay, "Cancelled from toolbar");
    player.run(cancelledPlay);
    wait([&] { return QFile::exists(cancelledPlay.userDir + "/started"); });
    player.stop();
    wait([&] { return queue.job(stopped)->state == CompilationJob::Cancelled; });
    // Closing the queue must not leave a controller or compiler orphaned.
    {
        CompilationQueue closing(nullptr, argv[1]);
        closing.enqueue(request("hold-closing"), "Closing");
        wait([&] { return QFile::exists(temp.path() + "/hold-closing/started"); });
    }
#ifdef Q_OS_UNIX
    const auto closedPid = read(temp.path() + "/hold-closing/started").split('\n')[0].toLongLong();
    CHECK(closedPid > 0 && kill(pid_t(closedPid), 0) == -1);
#endif
    // FailedToStart must finish a job, not stall all remaining requests.
    const QString invalid = temp.path() + "/non-executable";
    touch(invalid);
    CompilationQueue invalidQueue(nullptr, invalid);
    const auto bad = invalidQueue.enqueue(request("bad"), "Bad executable");
    const auto nextBad = invalidQueue.enqueue(request("bad-two"), "Next executable");
    wait([&] { return invalidQueue.job(bad)->isFinished() && invalidQueue.job(nextBad)->isFinished(); });
    CHECK(invalidQueue.job(bad)->state == CompilationJob::Failed && invalidQueue.activeCount() == 0);
    std::cout << "Parallel builds, budget, queue, per-job UI cancellation, process tree, retry, foreground and startup failure passed\n";
    return 0;
}
