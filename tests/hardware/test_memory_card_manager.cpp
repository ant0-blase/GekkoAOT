// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/MemoryCardManager.hpp"
#include "../../src/ShellStyle.hpp"
#include <QApplication>
#include <QEventLoop>
#include <QComboBox>
#include <QKeyEvent>
#include "../../src/SettingsStyle.hpp"
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <iostream>
#define CHECK(x) do{if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
// A stuck modal dialog otherwise ends as a silent CTest timeout: report the
// current step and the active window shortly before the 15 s limit.
static const char* g_step="startup";
static void armWatchdog(){
    auto* watchdog=new QTimer(qApp);watchdog->setSingleShot(true);
    QObject::connect(watchdog,&QTimer::timeout,[]{
        std::cerr<<"watchdog: stuck at step '"<<g_step<<"'\n";
        for(auto* w:QApplication::topLevelWidgets()){
            if(!w->isVisible())continue;
            std::cerr<<"  window "<<w->metaObject()->className()<<" name='"<<w->objectName().toStdString()
                     <<"' title='"<<w->windowTitle().toStdString()<<"' modal="<<w->isModal()<<"\n";
            for(auto* l:w->findChildren<QLabel*>())if(l->isVisible()&&!l->text().isEmpty())std::cerr<<"    label: "<<l->text().left(160).toStdString()<<"\n";
        }
        std::cerr.flush();std::exit(1);});
    watchdog->start(10000);
}
static MemoryCards::Save fixture(int n){
    MemoryCards::Save s;s.entry=QByteArray(64,'\xff');s.entry.replace(0,6,"GTEE01");s.entry.replace(8,32,QString("Save %1").arg(n).toLatin1().leftJustified(32,'\0'));s.entry[7]=2;
    auto p16=[&](int p,quint16 v){qToBigEndian(v,s.entry.data()+p);};auto p32=[&](int p,quint32 v){qToBigEndian(v,s.entry.data()+p);};p16(0x38,2);p16(0x30,2);p16(0x32,1);p32(0x28,3600);p32(0x2c,64);p32(0x3c,0);
    s.data=QByteArray(16384,'\0');s.data.replace(0,32,QString("Adventure %1").arg(n).toLatin1().leftJustified(32,'\0'));s.data.replace(32,32,QByteArray("Chapter 3 - Mountain").leftJustified(32,'\0',true));
    for(int i=64;i<8256;i+=2)qToBigEndian<quint16>(n%2?0x83e0:0xfc00,s.data.data()+i);return s;
}
int main(int argc,char** argv){
    QTemporaryDir temp;CHECK(temp.isValid());qputenv("XDG_CONFIG_HOME",(temp.path()+"/config").toUtf8());QApplication app(argc,argv);QApplication::setFont(shellBodyFont());QApplication::setPalette(launcherPalette());armWatchdog();
    const auto user=temp.path()+"/user/adventure",path=user+"/cards/MemoryCardA.raw";MemoryCards::Card card;QString error;CHECK(card.create(path,251,false,&error));CHECK(card.importSaves({fixture(1),fixture(2),fixture(3),fixture(4)},false,&error)&&card.commit(&error));
    {QFile sram(user+"/SRAM.raw");CHECK(sram.open(QIODevice::WriteOnly));sram.write(QByteArray(64,'\0'));}
    QMainWindow host;host.resize(1180,800);host.setCentralWidget(new QWidget);host.show();
    MemoryCardManager manager(temp.path(),user,{}, {},&host);presentShellDialog(manager,"Memory cards",false);manager.show();app.processEvents();
    auto* list=manager.findChild<QListWidget*>("memoryCardSaves");CHECK(list&&list->count()==4);CHECK(manager.findChild<QLabel*>("memorySaveTitle")->text()=="Adventure 1");CHECK(manager.findChild<QLabel*>("memorySaveInfo")->text().contains("2 blocks"));
    auto* cards=manager.findChild<QComboBox*>("memoryCardFiles");CHECK(cards&&cards->count()==1);
    g_step="card choice picker";
    QTimer::singleShot(30,&host,[&]{auto* popup=shellActiveDialog(&host);CHECK(popup&&popup->objectName()=="shellChoicePicker");CHECK(popup->width()<=620&&host.rect().contains(popup->geometry()));CHECK(!QApplication::activePopupWidget());auto* choices=popup->findChild<QListWidget*>("shellChoiceList");CHECK(choices&&choices->count()==1);QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(choices,&enter);});
    showShellChoices(cards);
    auto* search=manager.findChild<QLineEdit*>("memoryCardSearch");search->setText("Save 2");CHECK(list->item(0)->isHidden()&&!list->item(1)->isHidden());search->clear();
    const auto imported=temp.filePath("incoming.gci");CHECK(MemoryCards::Card::writeSave(imported,fixture(5),MemoryCards::SaveFormat::GCI,&error));
    g_step="import file dialog";
    // The file dialog lists its directory asynchronously (slower on Windows):
    // accept() is ignored until the selected file is known, so retry it.
    QTimer importer;importer.setInterval(40);
    QObject::connect(&importer,&QTimer::timeout,&host,[&]{auto* dialog=qobject_cast<QFileDialog*>(shellActiveDialog(&host));if(!dialog)return;dialog->selectFile(imported);QMetaObject::invokeMethod(dialog,"accept");});
    importer.start();
    manager.findChild<QPushButton*>("importMemorySaves")->click();importer.stop();CHECK(list->count()==5);CHECK(card.open(path,&error)&&card.saves().size()==5);
    // Cancel a delete, then perform one against this generated temporary card.
    g_step="cancelled delete";
    QTimer::singleShot(30,&host,[&]{auto* dialog=shellActiveDialog(&host);CHECK(dialog&&dialog!=&manager);dialog->reject();});manager.findChild<QPushButton*>("deleteMemorySave")->click();CHECK(list->count()==5);
    g_step="confirmed delete";
    QTimer::singleShot(30,&host,[&]{auto* dialog=shellActiveDialog(&host);CHECK(dialog&&dialog!=&manager);for(auto* button:dialog->findChildren<QPushButton*>())if(button->text()=="Delete"){button->click();return;}CHECK(false);});manager.findChild<QPushButton*>("deleteMemorySave")->click();CHECK(list->count()==4);
    CHECK(card.open(path,&error)&&card.saves().size()==4);CHECK(!QDir(path+".backups").entryList(QDir::Files).isEmpty());
    if(argc>2)CHECK(manager.openPath(QString::fromLocal8Bit(argv[2])));
    if(argc>1){QEventLoop settle;QTimer::singleShot(220,&settle,&QEventLoop::quit);settle.exec();manager.grab().save(QString::fromLocal8Bit(argv[1]));}manager.reject();
    g_step="running-card protection";
    MemoryCardManager running(temp.path(),user,user,{},&host);presentShellDialog(running,"Memory cards",false);running.show();app.processEvents();CHECK(!running.findChild<QPushButton*>("importMemorySaves")->isEnabled());CHECK(!running.findChild<QPushButton*>("deleteMemorySave")->isEnabled());CHECK(running.findChild<QLabel*>("memoryCardStatus")->text().contains("running game"));running.reject();
    std::cout<<"Memory card manager art/details, search, import, confirmed delete, backup and running-card protection passed\n";
}
