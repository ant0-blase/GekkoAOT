#include "../../src/LivingRoomView.hpp"
#include "../../src/SettingsStyle.hpp"
#include "../../src/ShellStyle.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            std::cerr << __LINE__ << ": " #x "\n";                                                           \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));
    app.setPalette(launcherPalette());
    CHECK(QFontInfo(shellDisplayFont(24)).family().contains("Doto"));
    for (const auto &icon :
         {"play", "pause", "square", "folder-open", "tv-minimal", "gamepad-2", "settings-2", "trash"})
        CHECK(!shellIcon(icon).isNull());
    QTemporaryDir temp;
    CHECK(temp.isValid());
    QList<LibraryEntry> games;
    const QStringList ids = {"GLME01", "GM4E01", "GOYE69", "GCOE52", "GHSE69", "GMS E01"};
    const QStringList titles = {
        "Luigi's Mansion",           "Mario Kart: Double Dash!!",        "GoldenEye: Rogue Agent",
        "Call of Duty: Finest Hour", "Harry Potter: Chamber of Secrets", "Super Mario Sunshine"};
    for (int i = 0; i < 6; ++i)
    {
        LibraryEntry e;
        e.path = temp.path() + QString("/game%1.dol").arg(i);
        QFile f(e.path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("fixture");
        e.title = titles[i];
        e.gameId = ids[i];
        e.publisher = "Nintendo · GameCube";
        e.region = "USA";
        e.lastPlayed = i < 2 ? 123 : 0;
        e.playSeconds = 3600 + i * 300;
        e.banner = QImage(96, 32, QImage::Format_RGB32);
        e.banner.fill(QColor(100 + i * 10, 100, 110));
        games << e;
    }
    LivingRoomView view;
    view.resize(1280, 800);
    view.setGames(games);
    view.show();
    view.activateWindow();
    app.processEvents();
    view.setCurrentGame(games[0].path);
    CHECK(view.visibleGames().size() == 6);
    CHECK(view.currentGame() == games[0].path);
    QString played, compiled;
    int resumes = 0, backs = 0, graphics = 0;
    QObject::connect(&view, &LivingRoomView::playRequested, [&](const QString &path) { played = path; });
    QObject::connect(&view, &LivingRoomView::compileRequested, [&](const QString &path) { compiled = path; });
    QObject::connect(&view, &LivingRoomView::resumeRequested, [&] { ++resumes; });
    QObject::connect(&view, &LivingRoomView::backRequested, [&] { ++backs; });
    QObject::connect(&view, &LivingRoomView::graphicsRequested, [&] { ++graphics; });
    auto *play = view.findChild<QPushButton *>("shellPlay");
    play->click();
    CHECK(played == games[0].path);
    view.findChild<QPushButton *>("shellCompile")->click();
    CHECK(compiled == games[0].path);
    view.setGameRunning(true, games[0].path);
    CHECK(play->text() == "Resume game");
    play->click();
    CHECK(resumes == 1);
    view.setCurrentGame(games[1].path);
    CHECK(play->text() == "Switch game");
    play->click();
    CHECK(played == games[1].path);
    view.setBuildStatus("Closing current game", 0);
    CHECK(!play->isEnabled());
    view.setCurrentGame(games[2].path);
    CHECK(!play->isEnabled());
    view.setBuildStatus({});
    view.setGameRunning(false);
    CHECK(play->isEnabled());
    auto *search = view.findChild<QLineEdit *>("shellSearch");
    search->setText("GoldenEye");
    CHECK(view.visibleGames() == QStringList{games[2].path});
    search->setText("absent");
    CHECK(view.visibleGames().isEmpty() && !play->isEnabled());
    search->clear();
    CHECK(view.visibleGames().size() == 6);
    view.findChild<QPushButton *>("shellRecent")->click();
    CHECK(view.visibleGames().size() == 2);
    view.findChild<QPushButton *>("shellLibrary")->click();
    CHECK(view.visibleGames().size() == 6);
    play->setFocus();
    view.navigate(Qt::Key_Down);
    CHECK(search->hasFocus());
    view.navigate(Qt::Key_Down);
    CHECK(view.currentGame() == games[0].path);
    view.navigate(Qt::Key_Right);
    CHECK(view.currentGame() == games[1].path);
    view.navigate(Qt::Key_Return);
    CHECK(played == games[1].path);
    view.openSettings();
    auto *graphic = view.findChild<QPushButton *>("shellGraphics");
    CHECK(graphic->isVisible());
    CHECK(!view.findChild<QPushButton*>("shellAppearance")->icon().isNull());
    CHECK(!view.findChild<QPushButton*>("shellDiscord")->icon().isNull());
    auto *settings=view.findChild<QPushButton*>("shellSettings");
    settings->click();CHECK(!graphic->isVisible());
    settings->click();CHECK(graphic->isVisible());
    const QPoint outside(20,120), global=view.mapToGlobal(outside);
    QMouseEvent outsidePress(QEvent::MouseButtonPress,outside,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&view,&outsidePress);
    QMouseEvent outsideRelease(QEvent::MouseButtonRelease,outside,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(&view,&outsideRelease);
    CHECK(!graphic->isVisible());view.openSettings();
    graphic->setFocus();
    view.navigate(Qt::Key_Return);
    CHECK(graphics == 1);
    view.navigate(Qt::Key_Escape);
    CHECK(!graphic->isVisible());
    view.navigate(Qt::Key_Escape);
    CHECK(backs == 1);
    view.setReducedMotion(true);
    CHECK(view.reducedMotion());
    view.setCurrentGame(games[0].path);
    for (const QSize size : {QSize(1280, 800), QSize(1280, 720), QSize(1920, 1080)})
    {
        view.resize(size);
        app.processEvents();
        for (const auto &name : {"shellPlay", "shellGameTitle", "shellQueue", "shellDesktop"})
        {
            auto *widget = view.findChild<QWidget *>(name);
            CHECK(widget && widget->isVisible());
            CHECK(view.rect().contains(QRect(widget->mapTo(&view, QPoint{}), widget->size())));
        }
        if (argc > 1)
        {
            QDir().mkpath(argv[1]);
            view.grab().save(QString(argv[1]) +
                             QString("/tv-%1x%2.png").arg(size.width()).arg(size.height()));
        }
    }
    view.openSettings();
    app.processEvents();
    if (argc > 1)
        view.grab().save(QString(argv[1]) + "/tv-settings.png");
    view.setGames({});
    CHECK(view.visibleGames().isEmpty() && !play->isEnabled());
    std::cout << "TV library, assets, search, recent games, launch, switch, resume, settings, controller "
                 "navigation and screen bounds passed\n";
}
