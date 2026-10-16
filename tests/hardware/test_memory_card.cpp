// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/MemoryCard.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdlib>
#include <iostream>
using namespace MemoryCards;
#define CHECK(x) do {if(!(x)){std::cerr<<__LINE__<<": " #x "\n";std::exit(1);}}while(false)
static void p16(QByteArray& b,int p,quint16 v){qToBigEndian(v,b.data()+p);}
static void p32(QByteArray& b,int p,quint32 v){qToBigEndian(v,b.data()+p);}
static quint16 be16(const QByteArray& b,int p){return qFromBigEndian<quint16>(b.constData()+p);}
static QByteArray read(const QString& p){QFile f(p);CHECK(f.open(QIODevice::ReadOnly));return f.readAll();}
static void write(const QString& p,const QByteArray& b){QFile f(p);CHECK(f.open(QIODevice::WriteOnly));CHECK(f.write(b)==b.size());}
static Save fixture(const QByteArray& name="Test save",int blocks=2){
    Save s;s.entry=QByteArray(64,'\xff');s.entry.replace(0,6,"GTEE01");s.entry.replace(8,32,name.leftJustified(32,'\0',true));s.entry[7]=2;
    p32(s.entry,0x28,3600);p32(s.entry,0x2c,64);p16(s.entry,0x30,2);p16(s.entry,0x32,1);p16(s.entry,0x38,quint16(blocks));p32(s.entry,0x3c,0);
    s.data=QByteArray(blocks*BlockSize,'\0');s.data.replace(0,32,QByteArray("Fixture game").leftJustified(32,'\0'));s.data.replace(32,32,QByteArray("Chapter 3").leftJustified(32,'\0'));
    for(int i=64;i<64+6144;i+=2)p16(s.data,i,0xfc00); // solid red RGB5A3 banner
    for(int i=64+6144;i<64+8192;i+=2)p16(s.data,i,0x83e0); // solid green icon
    return s;
}
static void fix(QByteArray& b,int start,int end,int dest){
    quint32 sum=0,inverse=0;for(int p=start;p<end;p+=2){const auto n=be16(b,p);sum+=n;inverse+=quint16(~n);}quint16 a=quint16(sum),c=quint16(inverse);p16(b,dest,a==0xffff?0:a);p16(b,dest+2,c==0xffff?0:c);
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);if(argc==2){Card card;QString error;if(!card.open(QString::fromLocal8Bit(argv[1]),&error)){std::cerr<<error.toStdString()<<"\n";return 1;}std::cout<<card.saves().size()<<" saves, "<<card.freeBlocks()<<" free blocks\n";for(const auto& save:card.saves())std::cout<<save.gameId().toStdString()<<" "<<save.filename().toStdString()<<" icons="<<save.icons().size()<<" banner="<<!save.banner().isNull()<<"\n";return 0;}QTemporaryDir temp;CHECK(temp.isValid());QString error;const auto path=temp.filePath("card.raw");
    Card card;CHECK(card.create(path,251,false,&error));CHECK(!card.create(path,251,false,&error));CHECK(card.capacity()==251&&card.freeBlocks()==251&&card.saves().isEmpty());
    const auto empty=read(path);CHECK(empty.size()==2*1024*1024);CHECK(be16(empty,0x22)==16);CHECK(be16(empty,3*BlockSize+6)==251);
    const auto save=fixture();CHECK(save.title()=="Fixture game"&&save.description()=="Chapter 3"&&save.filename()=="Test save");CHECK(save.gameId()=="GTEE01"&&save.modified().toSecsSinceEpoch()==946688400);
    CHECK(save.banner().size()==QSize(96,32)&&qRed(save.banner().pixel(0,0))==255);CHECK(save.icons().size()==1&&qGreen(save.icons()[0].image.pixel(0,0))==255);
    for(auto format:{SaveFormat::GCI,SaveFormat::GCS,SaveFormat::SAV}){
        const auto bytes=Card::encodeSave(save,format);Save decoded;CHECK(Card::decodeSave(bytes,decoded,&error));CHECK(decoded.entry==save.entry&&decoded.data==save.data);
        CHECK(!Card::decodeSave(bytes.chopped(1),decoded,&error));
        const auto filename=temp.filePath(QString::number(int(format))+".save");write(filename,bytes);Card individual;CHECK(individual.open(filename,&error)&&!individual.isImage()&&individual.saves().size()==1);
    }
    // An independent GameShark envelope uses an untrustworthy stored block
    // count; the reader must derive it from the actual payload length.
    QByteArray gcs(0x150,'\0');gcs.replace(0,6,"GCSAVE");auto gcsEntry=save.entry;p16(gcsEntry,0x38,99);gcs.replace(0x110,64,gcsEntry);gcs+=save.data;Save decoded;CHECK(Card::decodeSave(gcs,decoded,&error)&&decoded.blocks()==2);
    CHECK(card.importSaves({save},false,&error)&&card.freeBlocks()==249);CHECK(card.commit(&error));CHECK(!card.lastBackup().isEmpty()&&read(card.lastBackup())==empty);
    Card reopened;CHECK(reopened.open(path,&error)&&reopened.saves().size()==1&&reopened.saves()[0].data==save.data);CHECK(reopened.freeBlocks()==249);
    const auto before=read(path);CHECK(!reopened.importSaves({save},false,&error));CHECK(!reopened.importSaves({fixture("Too big",250)},false,&error));CHECK(reopened.saves().size()==1&&read(path)==before);
    auto replacement=fixture();replacement.data[100]='X';CHECK(reopened.importSaves({replacement},true,&error)&&reopened.commit(&error));CHECK(read(reopened.lastBackup())==before);
    Card stale;CHECK(stale.open(path,&error));CHECK(reopened.removeSaves({0},&error)&&reopened.commit(&error));CHECK(stale.removeSaves({0},&error)&&!stale.commit(&error));CHECK(read(path)==empty || be16(read(path),3*BlockSize+6)==251);
    // Read a valid secondary filesystem copy when the primary is damaged.
    write(path,before);auto damaged=before;damaged[BlockSize+10]^=1;write(path,damaged);CHECK(reopened.open(path,&error)&&reopened.saves().size()==1);
    damaged[2*BlockSize+10]^=1;write(path,damaged);CHECK(!reopened.open(path,&error));
    // A checksummed cyclic BAT is still invalid, including if duplicated.
    damaged=before;for(int bat:{3,4}){p16(damaged,bat*BlockSize+10,5);fix(damaged,bat*BlockSize+4,(bat+1)*BlockSize,bat*BlockSize);}write(path,damaged);CHECK(!reopened.open(path,&error));
    // Directory/BAT cross-links must never be imported or overwritten.
    damaged=before;for(int d:{1,2}){damaged.replace(d*BlockSize+64,64,damaged.mid(d*BlockSize,64));fix(damaged,d*BlockSize,(d+1)*BlockSize-4,(d+1)*BlockSize-4);}write(path,damaged);CHECK(!reopened.open(path,&error));
    // MCI is a card image preceded by its original 64-byte header.
    const auto mci=temp.filePath("wrapped.mci");write(mci,QByteArray(64,'M')+before);CHECK(reopened.open(mci,&error)&&reopened.saves().size()==1);CHECK(reopened.removeSaves({0},&error)&&reopened.commit(&error));CHECK(read(mci).left(64)==QByteArray(64,'M'));
    CHECK(reopened.exportImage(temp.filePath("converted.gcp"),&error));Card converted;CHECK(converted.open(temp.filePath("converted.gcp"),&error)&&converted.capacity()==251);
    // Invalid image offsets are metadata omissions, not an out-of-bounds read.
    auto bad=save;p32(bad.entry,0x2c,0xfffffffe);p32(bad.entry,0x3c,0xfffffffe);CHECK(bad.banner().isNull()&&bad.icons().isEmpty()&&bad.title().isEmpty());
    auto japanese=save;japanese.entry[3]='J';japanese.entry.replace(8,32,QByteArray::fromHex("835a815b8375").leftJustified(32,'\0'));CHECK(japanese.filename()==QString::fromUtf8("セーブ"));
    // Indexed GX textures use 8x4 tiles and a palette after the icon data.
    auto ci=save;ci.entry[7]=0;p16(ci.entry,0x30,1);p32(ci.entry,0x2c,64);ci.data.fill('\0');std::fill(ci.data.begin()+64,ci.data.begin()+1088,'\x03');p16(ci.data,1088+6,0x801f);CHECK(ci.icons().size()==1&&qBlue(ci.icons()[0].image.pixel(31,31))==255);
    for(int n:{59,123,507,1019,2043}){Card sized;CHECK(sized.create(temp.filePath(QString::number(n)+".raw"),n,true,&error));CHECK(sized.capacity()==n&&sized.japanese());}
    // Arbitrary bytes, truncated envelopes and huge block counts are rejected.
    for(int n=0;n<500;++n){Save invalid;CHECK(!Card::decodeSave(QByteArray(n,'\xff'),invalid,&error));}
    std::cout<<"RAW/GCP/MCI, GCI/GCS/SAV, save art/text, allocation validation, backups and concurrent edits passed\n";
}
