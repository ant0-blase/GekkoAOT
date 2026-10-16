// Small real subprocess used to exercise scheduling, partial output and tree cancellation.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QThread>
#include <cstdio>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif
static void write(const QString& path, const QByteArray& data) {
    QFile file(path); if (file.open(QIODevice::WriteOnly)) { file.write(data); file.close(); }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2 || (QString(argv[1]) != "compile" && QString(argv[1]) != "run")) return 99;
    const QString slug = qEnvironmentVariable("GEKKOAOT_GAME");
    const QString prefix = qEnvironmentVariable("GEKKOAOT_USER_DIR");
    QDir().mkpath(prefix);
#ifdef Q_OS_UNIX
    if (slug.contains("resistant")) {
        signal(SIGTERM, SIG_IGN);
        const pid_t child = fork();
        if (child == 0) { for (;;) pause(); }
        write(prefix + "/child", QByteArray::number(child));
    }
#endif
    write(prefix + "/started", QByteArray::number(QCoreApplication::applicationPid()) + "\n" +
        qgetenv("GEKKOAOT_JOBS") + "\n" + qgetenv("GEKKOAOT_BACKEND") + "\n" + qgetenv("GEKKOAOT_NATIVE_HW") +
        "\n" + qgetenv("GEKKOAOT_PROFILE") + "\n" + QByteArray(argv[1]));
    std::fputs("GEKKOAOT_PRO", stdout); std::fflush(stdout);
    QThread::msleep(25);
    std::fputs("GRESS=47|Compiling fixture\n[1/3] Emitting LLVM object\n", stdout); std::fflush(stdout);
    if (slug.contains("fail")) {
        if (!QFile::exists(prefix + "/failed-once")) {
            write(prefix + "/failed-once", "1");
            std::fputs("fixture deliberate failure\n", stderr); return 7;
        }
        return 0;
    }
    if (slug.contains("success")) return 0;
    while (!QFile::exists(prefix + "/release")) QThread::msleep(10);
    if (slug.contains("runtime")) {
        std::fputs("GEKKOAOT_STAGE=runtime\nGEKKOAOT_PROGRESS=100|Game running\n", stdout); std::fflush(stdout);
        while (!QFile::exists(prefix + "/close")) QThread::msleep(10);
    }
    return 0;
}
