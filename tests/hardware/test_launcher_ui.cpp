#include "../../src/DiscMetadata.hpp"
#include "../../src/GameLibrary.hpp"
#include "../../src/MainWindow.hpp"
#include "../../src/LivingRoomView.hpp"
#include "../../src/ShellGamepad.hpp"
#include "../../src/ShellStyle.hpp"
#include "../../src/ShellNavigation.hpp"
#include "../../src/ShellTitleBar.hpp"
#include "../../src/GameCard.hpp"
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QToolBar>
#include <QToolButton>
#include <QLayout>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QMenu>
#include <QProcess>
#include <QEventLoop>
#include <QThread>
#include <QFileDialog>
#include <QAbstractButton>
#ifdef GEKKOAOT_LAUNCHER_SDL3
#include <SDL3/SDL.h>
#endif
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDockWidget>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QHeaderView>
#include <cstdlib>
#include <iostream>
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            std::cerr << __LINE__ << ": " #x "\n";                                                           \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (false)
static void write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    CHECK(f.open(QIODevice::WriteOnly) && f.write(data) == data.size());
}
static int realGameSmoke(int argc,char** argv)
{
    CHECK(argc==3);
    const QString work=argv[2];
    qputenv("GEKKOAOT_STATE_DIR",(work+"/state").toUtf8());
    qputenv("XDG_CONFIG_HOME",(work+"/config").toUtf8());
    qputenv("XDG_DATA_HOME",(work+"/data").toUtf8());
    qputenv("GEKKOAOT_LAUNCHER_GAMEPAD_NAME","Private UX test pad");
    write(work+"/native-game.log",{});
    QApplication app(argc,argv);app.setOrganizationName("GekkoAOT");app.setApplicationName("GekkoAOT");
    QSettings("GekkoAOT", "GekkoAOT").setValue("discord/enabled", false);
    app.setStyle(QStyleFactory::create("Fusion"));
    auto wait=[&](auto condition,int timeout=30000){QElapsedTimer clock;clock.start();while(!condition() && clock.elapsed()<timeout){app.processEvents();QThread::msleep(8);}CHECK(condition());};
    auto settle=[&](int ms=180){QElapsedTimer clock;clock.start();while(clock.elapsed()<ms){app.processEvents();QThread::msleep(5);}};
    auto capture=[&](const QString& name){QDir().mkpath(work+"/verified");QProcess::execute("gamescopectl",{"screenshot",work+"/verified/"+name+".png"});settle();};
    MainWindow window;window.show();settle(500);
    auto* game=window.gameWindow();CHECK(game);
    auto* viewport=game->findChild<QWidget*>("desktopGameViewport");CHECK(viewport);
    auto* pause=window.findChild<QAction*>("pauseGame");CHECK(pause);
    const QRect launcherGeometry=window.geometry();const auto launcherState=window.windowState();
    window.findChild<QToolButton*>("primaryPlay")->click();
    wait([&]{QFile file(work+"/native-game.log");return game->isVisible() && pause->isEnabled() && file.open(QIODevice::ReadOnly) && file.readAll().contains("GEKKOAOT_AURORA_GX_V001=1");});
    settle(1500);CHECK(!window.livingRoomVisible() && window.windowState()==launcherState && window.geometry()==launcherGeometry);
    capture("01-desktop-game");std::cout<<"Real cached game started in a separate window\n"<<std::flush;
    auto* bar=game->findChild<QWidget*>("shellTitleBar");CHECK(bar && bar->isVisible());
    const auto old=game->pos();const QPoint local(160,14),global=bar->mapToGlobal(local);
    QMouseEvent press(QEvent::MouseButtonPress,local,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(bar,&press);
    QMouseEvent move(QEvent::MouseMove,local+QPoint(40,30),global+QPoint(40,30),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(bar,&move);
    QMouseEvent release(QEvent::MouseButtonRelease,local+QPoint(40,30),global+QPoint(40,30),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(bar,&release);
    settle();CHECK(game->pos()!=old);std::cout<<"Game title bar drag passed\n"<<std::flush;
    for(auto* b:bar->findChildren<QToolButton*>())if(b->accessibleName()=="Fullscreen")b->click();
    wait([&]{return game->isFullScreen();});settle();
    CHECK(!bar->isVisible() && viewport->geometry()==game->centralWidget()->rect());
    CHECK(window.windowState()==launcherState && window.geometry()==launcherGeometry);
    capture("02-game-fullscreen");std::cout<<"Fullscreen button: no title bar, no margins, launcher unchanged\n"<<std::flush;
    auto* shortcut=window.findChild<QShortcut*>("fullscreenShortcut");
    auto key=[&](const QString& chord){CHECK(QProcess::execute("xdotool",{"key","--window",QString::number(game->winId()),chord})==0);settle();};
    key("F11");CHECK(!game->isFullScreen() && bar->isVisible());
    key("alt+Return");CHECK(game->isFullScreen() && !window.isFullScreen());
    window.openLivingRoom();settle();CHECK(window.isFullScreen() && window.livingRoomVisible());
    QTimer::singleShot(200,&window,[&]{
        auto* panel=shellActiveDialog(&window);CHECK(panel && panel->objectName()=="graphicsSettings");
        panel->findChild<QTabWidget*>()->setCurrentIndex(1);
        auto* value=panel->findChild<QDoubleSpinBox*>("brightness");value->setValue(1.12);
        QTimer::singleShot(200,panel,[&,panel,value]{
            CHECK(game->isFullScreen() && window.isFullScreen());
            CHECK(viewport->geometry()==game->centralWidget()->rect());
            auto* label=value->parentWidget()->findChild<QLabel*>("shellStepperValue");
            CHECK(label && label->isVisible() && label->text().contains("12"));
            capture("03-graphics-preview");panel->accept();
        });
    });
    auto* tv=window.findChild<LivingRoomView*>("livingRoom");tv->openSettings();tv->findChild<QPushButton*>("shellGraphics")->click();settle();
    CHECK(game->isFullScreen());
    tv->resumeRequested();settle();
    CHECK(!window.livingRoomVisible() && window.windowState()==launcherState && window.geometry()==launcherGeometry && game->isFullScreen());
    capture("04-resumed-game");std::cout<<"Graphics preview and Home/resume preserve the fullscreen surface\n"<<std::flush;
    key("F11");CHECK(!game->isFullScreen());
    CHECK(viewport->geometry().left()==0 && viewport->geometry().right()==game->centralWidget()->rect().right());
    game->close();wait([&]{return !pause->isEnabled();});
    CHECK(window.isVisible() && !window.isFullScreen());
    // Start in TV this time, then leave fullscreen. The game must become a
    // separate desktop window without changing its native render surface.
    write(work+"/native-game.log",{});window.openLivingRoom();settle();
    tv->findChild<QPushButton*>("shellPlay")->click();
    wait([&]{QFile file(work+"/native-game.log");return game->isVisible() && pause->isEnabled() && file.open(QIODevice::ReadOnly) && file.readAll().contains("GEKKOAOT_AURORA_GX_V001=1");});
    settle(1000);CHECK(game->isFullScreen());const WId nativeSurface=viewport->winId();
    key("F11");CHECK(!game->isFullScreen() && !window.isFullScreen() && !window.livingRoomVisible());
    CHECK(window.findChild<QWidget*>("gameLibrary")->isVisible() && viewport->winId()==nativeSurface);
    capture("05-tv-to-desktop");std::cout<<"TV fullscreen exits to separate game + library, same native surface\n"<<std::flush;
    game->close();wait([&]{return !pause->isEnabled();});
    std::cout<<"REAL GAME UX SMOKE PASSED\n"<<std::flush;return 0;
}
int main(int argc, char **argv)
{
    if(argc>1 && QByteArray(argv[1])=="--real-game")return realGameSmoke(argc,argv);
    QTemporaryDir temporary;
    CHECK(temporary.isValid());
    qputenv("GEKKOAOT_STATE_DIR", temporary.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (temporary.path() + "/config").toUtf8());
    QApplication app(argc, argv);
    app.setOrganizationName("GekkoAOT tests");
    app.setApplicationName("Launcher UI");
    QSettings("GekkoAOT", "GekkoAOT").setValue("discord/enabled", false);
    app.setStyle(QStyleFactory::create("Fusion"));
    const QString game = temporary.path() + "/fixture/sys/main.dol";
    QByteArray boot(0x440, 0);
    boot.replace(0, 6, "GTEE01");
    boot.replace(0x20, 6, "Header");
    QByteArray banner(0x1960, 0);
    banner.replace(0, 4, "BNR1");
    banner.replace(0x1860, 13, "Fixture title");
    banner.replace(0x18a0, 9, "Publisher");
    banner.replace(0x18e0, 7, "Comment");
    for (int i = 0x20; i < 0x1820; i += 2)
    {
        banner[i] = char(0xfc);
        banner[i + 1] = 0;
    }
    write(game, QByteArray(0x100, 0));
    write(temporary.path() + "/fixture/sys/boot.bin", boot);
    write(temporary.path() + "/fixture/files/opening.bnr", banner);
    CHECK(decodeGameCubeBanner(banner).size() == QSize(96, 32));
    CHECK(decodeGameCubeBanner("bad").isNull());
    qputenv("GEKKOAOT_LAUNCHER_GAMEPAD_NAME","Launcher modal integration pad");
    MainWindow window;
    CHECK(app.font().family().contains("Google Sans Flex") && app.font().weight()==QFont::Medium);
    window.show();
    auto *library = dynamic_cast<GameLibrary *>(window.findChild<QWidget *>("gameLibrary"));
    CHECK(library);
    CHECK(!library->testAttribute(Qt::WA_NativeWindow));
    CHECK(!library->findChild<QWidget*>("libraryActions"));
    CHECK(library->findChild<QLabel*>("launcherVersion")->text()==QString("GekkoAOT v%1").arg(GEKKOAOT_VERSION));
    auto* toolbar=window.findChild<QToolBar*>("desktopNavigation");
    auto* transport=toolbar->findChild<QWidget*>("desktopTransport");
    CHECK(qAbs(transport->mapTo(toolbar,transport->rect().center()).x()-toolbar->rect().center().x())<=2);
    auto* playButton=toolbar->findChild<QToolButton*>("primaryPlay");
    CHECK(playButton->toolButtonStyle()==Qt::ToolButtonIconOnly);
    CHECK(!window.findChild<QWidget*>("shellTitleBar")->isVisible());
    CHECK(toolbar->findChild<QToolButton*>("windowClose"));
    auto* menuButton=toolbar->findChild<QToolButton*>("desktopMenu");
    auto* logo=toolbar->findChild<QLabel*>("desktopProjectLogo");
    CHECK(logo->mapTo(toolbar,QPoint{}).x()<menuButton->mapTo(toolbar,QPoint{}).x());
    for(auto* button:toolbar->findChildren<QToolButton*>())CHECK(!button->defaultAction() || button->defaultAction()->text()!="Open Game…");
    library->addGame(game);
    QTimer timer;
    int attempts = 0;
    QObject::connect(
        &timer, &QTimer::timeout,
        [&]
        {
            CHECK(++attempts < 50);
            if (library->metadata(game)["gameId"].toString().isEmpty())
                return;
            timer.stop();
            auto meta = library->metadata(game);
            CHECK(meta["gameId"].toString() == "GTEE01");
            CHECK(meta["title"].toString() == "Fixture title");
            CHECK(meta["publisher"].toString() == "Publisher");
            CHECK(meta["comment"].toString() == "Comment");
            const QString cache = discMetadataCachePath(temporary.path(), game);
            CHECK(QFileInfo::exists(cache + "/sys/boot.bin"));
            CHECK(!QFileInfo::exists(cache + "/sys/main.dol"));
            library->selectGame(game);
            library->recordLaunch(game);
            library->addPlayTime(game, 123);
            CHECK(library->metadata(game)["playSeconds"].toString() == "123");
            auto *tree = library->findChild<QTreeWidget *>("libraryGames");
            CHECK(tree && tree->topLevelItemCount() == 1);
            CHECK(tree->selectionMode() == QAbstractItemView::ExtendedSelection);
            const QString second = temporary.path() + "/second.dol";
            write(second, QByteArray(0x100, 0));
            library->addGame(second);
            for (int i = 0; i < tree->topLevelItemCount(); ++i) tree->topLevelItem(i)->setSelected(true);
            CHECK(library->selectedPaths().size() == 2);
            library->selectGame(game);
            CHECK(library->selectedPaths().size() == 2); // Selection callback must not collapse the batch.
            for(int i=0;i<18;++i){const auto file=temporary.path()+QString("/Navigation %1.dol").arg(i,2,10,QChar('0'));write(file,QByteArray(0x100,0));library->addGame(file);}
            auto* dock = window.findChild<QDockWidget*>("compilationDock");
            CHECK(dock && !dock->isVisible());
            CHECK(dock->palette().color(QPalette::WindowText).lightness() > 180);
            CHECK(app.palette().color(QPalette::Base).lightness() < 80);
            CHECK(app.palette().color(QPalette::Text).lightness() > 180);
            auto* parallel = dock->findChild<QSpinBox*>("parallelCompilations");
            CHECK(parallel && parallel->minimum() == 1 && parallel->maximum() == 8);
            auto *search = library->findChild<QLineEdit *>("librarySearch");
            search->setText("missing");
            CHECK(tree->topLevelItem(0)->isHidden());
            search->clear();
            CHECK(!tree->topLevelItem(0)->isHidden());
            {
                GameLibrary restored(temporary.path(), {});
                CHECK(restored.metadata(game)["playSeconds"].toString() == "123");
            }
            const QString prefix = argc > 1 ? QString(argv[1]) : QString{};
            {QEventLoop settle;QTimer::singleShot(120,&settle,&QEventLoop::quit);settle.exec();}
            if (!prefix.isEmpty()) window.grab().save(prefix + "library.png");
            auto *advanced = window.findChild<QDialog *>("advancedSettings");
            CHECK(advanced);
            auto *tabs = advanced->findChild<QTabWidget *>("advancedTabs");
            CHECK(tabs && tabs->count() == 3);
            presentShellDialog(*advanced,"Advanced",false);advanced->show();
            for (int i = 0; i < 3; ++i)
            {
                tabs->setCurrentIndex(i);
                app.processEvents();
                if (!prefix.isEmpty())
                    advanced->grab().save(prefix + "advanced-" + QString::number(i) + ".png");
            }
            advanced->hide();
            auto *compile = window.findChild<QDialog *>("compilationProgress");
            CHECK(compile && compile->isWindow() && compile->parentWidget() == &window);
            auto *pause = window.findChild<QAction *>("pauseGame");
            CHECK(pause && pause->isCheckable() && !pause->isEnabled());
            window.openLivingRoom();
            app.processEvents();
            CHECK(window.livingRoomVisible() && window.isFullScreen());
            auto* tv=window.findChild<LivingRoomView*>("livingRoom");
            CHECK(tv && tv->isVisible());
            CHECK(!dock->isVisible());
            CHECK(!window.windowIcon().isNull());
            tv->openSettings();
            tv->findChild<QPushButton*>("shellAdvanced")->click();app.processEvents();
            CHECK(advanced->isVisible() && advanced->property("tvMode").toBool());
            CHECK(!advanced->isWindow() && advanced->window()==&window);
            int oldTab=tabs->currentIndex();
            auto* monitor=window.findChild<ShellGamepad*>();CHECK(monitor);
            auto navigate=[&](int key){QMetaObject::invokeMethod(monitor,"navigationKey",Qt::DirectConnection,Q_ARG(int,key));app.processEvents();};
            QMetaObject::invokeMethod(monitor,"navigationKey",Qt::DirectConnection,Q_ARG(int,Qt::Key_PageDown));app.processEvents();
            CHECK(tabs->currentIndex()==(oldTab+1)%tabs->count());
            if(!prefix.isEmpty())advanced->grab().save(prefix+"tv-advanced.png");
            QMetaObject::invokeMethod(monitor,"navigationKey",Qt::DirectConnection,Q_ARG(int,Qt::Key_Escape));app.processEvents();CHECK(!advanced->isVisible());
#ifdef GEKKOAOT_LAUNCHER_SDL3

            // The real SDL polling timer must remain live while a controller
            // confirmation enters QDialog::exec (the original deadlock).
            SDL_VirtualJoystickDesc description;SDL_INIT_INTERFACE(&description);
            description.type=SDL_JOYSTICK_TYPE_GAMEPAD;description.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;description.naxes=SDL_GAMEPAD_AXIS_COUNT;
            description.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;description.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;description.name="Launcher modal integration pad";
            auto id=SDL_AttachVirtualJoystick(&description);CHECK(id);auto* pad=SDL_OpenJoystick(id);CHECK(pad);monitor->poll();
            auto virtualButton=[&](int button,bool down){CHECK(SDL_SetJoystickVirtualButton(pad,button,down));SDL_UpdateJoysticks();};
            for(const auto& target:QStringList{"shellGraphics","shellInput","shellOpen","shellDiscord","shellMemoryCards"}) {
                bool opened=false,closedByPad=false;
                tv->openSettings();
                if(target=="shellOpen") {
                    tv->findChild<QPushButton*>("shellSettingsBack")->click();
                }
                tv->findChild<QPushButton*>(target)->setFocus();window.activateWindow();app.processEvents();
                monitor->setNavigationEnabled(true);
                QEventLoop loop;
                QTimer::singleShot(10,&loop,[&]{virtualButton(SDL_GAMEPAD_BUTTON_SOUTH,true);});
                QTimer::singleShot(55,&loop,[&]{virtualButton(SDL_GAMEPAD_BUTTON_SOUTH,false);});
                QTimer::singleShot(250,&loop,[&]{
                    auto* modal=shellActiveDialog(&window);
                    opened=modal && modal->property("tvMode").toBool();
                    if(modal && target=="shellGraphics") {
                        auto* graphicsTabs=modal->findChild<QTabWidget*>();CHECK(graphicsTabs);
                        navigate(Qt::Key_Down);CHECK(QApplication::focusWidget()->objectName()=="shellTab0");
                        navigate(Qt::Key_Right);CHECK(QApplication::focusWidget()->objectName()=="shellTab1");
                        navigate(Qt::Key_Return);CHECK(graphicsTabs->currentIndex()==1);
                        CHECK(modal->findChild<QAbstractSpinBox*>("brightness")->parentWidget()->objectName()=="shellStepper");
                        auto* value=modal->findChild<QAbstractSpinBox*>("brightness")->parentWidget()->findChild<QLabel*>("shellStepperValue");
                        CHECK(value && value->isVisible() && value->width()>=54 && !value->text().isEmpty());
                        auto* model=modal->findChild<QDoubleSpinBox*>("brightness");
                        model->parentWidget()->findChild<QPushButton*>("stepperPlus")->click();
                        CHECK(value->text()==model->text() && model->value()>1.0);
                        navigate(Qt::Key_Down);
                        for(int i=0;i<15;++i)navigate(Qt::Key_Down);
                        CHECK(qobject_cast<QDialogButtonBox*>(QApplication::focusWidget()->parentWidget()));
                        CHECK(window.isFullScreen() && window.livingRoomVisible());
                    }
                    if(modal && target=="shellOpen")CHECK(modal->property("shellSnapshotTransition").toBool());
                    if(modal && !prefix.isEmpty())modal->grab().save(prefix+"tv-"+target+".png");
                    virtualButton(SDL_GAMEPAD_BUTTON_EAST,true);
                    // Schedule release relative to press. Screenshot/layout
                    // work can delay the inspection timer on a busy runner.
                    QTimer::singleShot(60,&loop,[&]{virtualButton(SDL_GAMEPAD_BUTTON_EAST,false);});
                });
                QTimer closed;
                closed.setInterval(30);
                QObject::connect(&closed,&QTimer::timeout,&loop,[&]{if(opened && !shellActiveDialog(&window)){closedByPad=true;loop.quit();}});
                closed.start();
                QTimer::singleShot(2000,&loop,[&]{if(auto* modal=shellActiveDialog(&window))modal->close();loop.quit();});
                loop.exec();if(!(opened&&closedByPad))std::cerr<<"target="<<target.toStdString()<<" opened="<<opened<<" closed="<<closedByPad<<" active="<<(shellActiveDialog(&window)?shellActiveDialog(&window)->windowTitle().toStdString():"none")<<"\n";CHECK(opened && closedByPad);
            }
            SDL_CloseJoystick(pad);CHECK(SDL_DetachVirtualJoystick(id));monitor->poll();
#endif
            tv->findChild<QPushButton*>("shellQueue")->click();app.processEvents();
            auto* queue=window.findChild<QDialog*>("compilationQueuePanel");CHECK(queue && queue->isVisible() && queue->property("tvMode").toBool());
            if(!prefix.isEmpty())queue->grab().save(prefix+"tv-compilations.png");
            queue->close();app.processEvents();

            tv->findChild<QPushButton*>("shellDesktop")->click();
            app.processEvents();
            CHECK(!window.livingRoomVisible() && !window.isFullScreen());
            window.activateWindow();app.processEvents();
            QMetaObject::invokeMethod(monitor,"navigationKey",Qt::DirectConnection,Q_ARG(int,Qt::Key_PageDown));app.processEvents();
            CHECK(QApplication::focusWidget() && QApplication::focusWidget()->objectName()=="desktopGameCard");
            auto* initial=QApplication::focusWidget();
            QMetaObject::invokeMethod(monitor,"navigationKey",Qt::DirectConnection,Q_ARG(int,Qt::Key_Right));app.processEvents();
            CHECK(QApplication::focusWidget()!=initial);
            CHECK(dynamic_cast<GameCard*>(QApplication::focusWidget()));
            // Traverse more rows than fit onscreen, then climb to the search
            // and toolbar. Metadata/selection refresh must not move us back.
            for(int i=0;i<10;++i)navigate(Qt::Key_Down);
            auto* bottom=QApplication::focusWidget();
            {QEventLoop settle;QTimer::singleShot(180,&settle,&QEventLoop::quit);settle.exec();}
            CHECK(QApplication::focusWidget()==bottom);
            for(int i=0;i<15 && QApplication::focusWidget()!=search;++i)navigate(Qt::Key_Up);
            CHECK(QApplication::focusWidget()==search);
            auto* scroll=library->findChild<QScrollArea*>("libraryCardView");
            CHECK(scroll && scroll->verticalScrollBar()->value()<40);
            navigate(Qt::Key_Up);CHECK(QApplication::focusWidget()->objectName()=="desktopMenu");
            bool reachedMenu=false,reachedTV=false;
            for(int i=0;i<14;++i){reachedMenu|=QApplication::focusWidget()->objectName()=="desktopMenu";auto* b=qobject_cast<QToolButton*>(QApplication::focusWidget());reachedTV|=b && b->defaultAction() && b->defaultAction()->objectName()=="tvMode";navigate(Qt::Key_Right);}
            CHECK(reachedMenu && reachedTV && !window.findChild<QToolButton*>("desktopSettings"));
            auto* menuButton=window.findChild<QToolButton*>("desktopMenu");menuButton->click();app.processEvents();
            auto* menuPanel=shellActiveDialog(&window);CHECK(menuPanel && menuPanel->objectName()=="navigationPanel");
            CHECK(!menuPanel->findChild<QWidget*>("shellDialogHeader")->isVisible());
            CHECK((menuPanel->property("shellRevealOrigin").toPoint()-menuPanel->mapFromGlobal(menuButton->mapToGlobal(menuButton->rect().center()))).manhattanLength()<=2);
            bool hasOpen=false;for(auto* button:menuPanel->findChildren<QPushButton*>())hasOpen|=button->text()=="Open game…";CHECK(hasOpen);
            {QEventLoop settle;QTimer::singleShot(260,&settle,&QEventLoop::quit);settle.exec();}
            if(!prefix.isEmpty())window.grab().save(prefix+"desktop-menu.png");
            menuButton->click();app.processEvents();CHECK(!shellActiveDialog(&window));
            navigate(Qt::Key_Down);CHECK(QApplication::focusWidget()==search);
            // Search opens a controller keyboard. Enter 2, then reach Done
            // entirely with directional movement and confirmation.
            QTimer::singleShot(40,&window,[&]{
                auto* keyboard=shellActiveDialog(&window);CHECK(keyboard && keyboard->objectName()=="shellKeyboard");
                CHECK(QApplication::focusWidget()->objectName()=="keyboardKey1");
                navigate(Qt::Key_Right);navigate(Qt::Key_Return);
                for(int i=0;i<4;++i)navigate(Qt::Key_Down);
                for(int i=0;i<4;++i)navigate(Qt::Key_Right);
                auto* done=qobject_cast<QPushButton*>(QApplication::focusWidget());CHECK(done && done->text()=="Done");navigate(Qt::Key_Return);
            });
            navigate(Qt::Key_Return);CHECK(search->text()=="2");search->clear();app.processEvents();
            // Fullscreen decoration belongs to the game window only.
            auto* gameWindow=window.gameWindow();CHECK(gameWindow);
            gameWindow->show();app.processEvents();
            auto* bar=dynamic_cast<ShellTitleBar*>(gameWindow->findChild<QWidget*>("shellTitleBar"));CHECK(bar && bar->isVisible());
            for(auto* b:bar->findChildren<QToolButton*>())if(b->accessibleName()=="Fullscreen")b->click();
            app.processEvents();CHECK(gameWindow->isFullScreen() && !window.isFullScreen() && !bar->isVisible());
            auto* viewport=gameWindow->findChild<QWidget*>("desktopGameViewport");
            CHECK(viewport->geometry()==gameWindow->centralWidget()->rect());
            gameWindow->showNormal();app.processEvents();CHECK(bar->isVisible());
            CHECK(gameWindow->centralWidget()->layout()->contentsMargins()==QMargins{});gameWindow->hide();
            window.activateWindow();app.processEvents();
            auto* fullscreen=window.findChild<QShortcut*>("fullscreenShortcut");CHECK(fullscreen);
            QMetaObject::invokeMethod(fullscreen,"activated");app.processEvents();CHECK(window.isFullScreen() && !window.livingRoomVisible());
            QMetaObject::invokeMethod(fullscreen,"activated");app.processEvents();CHECK(!window.isFullScreen());
            // Change view through the actual context menu, without another
            // action bar competing with game-level actions.
            bool foundView=false;
            QTimer::singleShot(260,&window,[&]{
                auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());CHECK(menu && menu->parentWidget()==&window);
                CHECK(menu->width()>=260);for(auto* action:menu->actions())if(!action->isSeparator())CHECK(menu->actionGeometry(action).height()>=40);
                if(!prefix.isEmpty())menu->grab().save(prefix+"desktop-context.png");
                for(auto* action:menu->actions())if(action->objectName()=="libraryViewToggle") {
                    foundView=true;menu->setActiveAction(action);
                    QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(menu,&enter);break;
                }
            });
            auto* contextCard=library->findChild<QWidget*>("desktopGameCard");CHECK(contextCard);
            QMetaObject::invokeMethod(contextCard,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,QPoint(80,60)));
            app.processEvents();CHECK(foundView);
            CHECK(tree->isVisible() && tree->header()->height()>=36);
            if(!prefix.isEmpty())window.grab().save(prefix+"desktop-list.png");
            std::cout << "Library, embedded overlays, virtual controller in Graphics/Input/file browser, tabs and desktop/TV transitions passed\n";
            app.quit();
        });
    timer.start(50);
    return app.exec();
}
