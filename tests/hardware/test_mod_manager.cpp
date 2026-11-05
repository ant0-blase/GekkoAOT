// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/ModManagerDialog.hpp"
#include "../../src/MemoryCard.hpp"
#include "../../src/MemoryCardSaveIcons.hpp"
#include <QApplication>
#include <QDir>
#include <QComboBox>
#include <QtEndian>
#include <QFile>
#include <QTemporaryDir>
#include <cassert>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QTemporaryDir temp;
  assert(temp.isValid());
  QString why;
  // Save icon wins; when absent, opening.bnr from the matching disc cache wins.
  // A third game without either source keeps the generic icon.
  const QString cardPath = temp.filePath("user/hp/cards/MemoryCardA.raw");
  MemoryCards::Card card;
  assert(card.create(cardPath, 251, false, &why));
  MemoryCards::Save save;
  save.entry = QByteArray(64, char(0xff));
  save.entry.replace(0, 6, "GAZE69");
  save.entry.replace(8, 32, QByteArray("HP_save").leftJustified(32, '\0'));
  save.entry[7] = 2; // RGB5A3 banner and icon.
  qToBigEndian<quint16>(2, save.entry.data() + 0x30);
  qToBigEndian<quint16>(1, save.entry.data() + 0x32);
  qToBigEndian<quint16>(2, save.entry.data() + 0x38);
  qToBigEndian<quint32>(64, save.entry.data() + 0x2c);
  save.data = QByteArray(2 * MemoryCards::BlockSize, '\0');
  for (int offset = 64; offset < 64 + 6144; offset += 2)
    qToBigEndian<quint16>(0xfc00, save.data.data() + offset);
  for (int offset = 64 + 6144; offset < 64 + 8192; offset += 2)
    qToBigEndian<quint16>(0x83e0, save.data.data() + offset);
  assert(card.importSaves({save}, false, &why) && card.commit(&why));
  // Synthetic cached metadata, using the real BNR1 image format. The save
  // for GAZE69 is green, its BNR is red; G4BE08 has only a blue BNR.
  const auto makeCachedBanner = [&](const QString &cache, const QByteArray &gameId,
                                    quint16 color) {
    const QString folder = temp.filePath("library/discs/" + cache);
    assert(QDir().mkpath(folder + "/sys") && QDir().mkpath(folder + "/meta"));
    QFile boot(folder + "/sys/boot.bin");
    assert(boot.open(QIODevice::WriteOnly));
    QByteArray bootBytes(0x440, '\0');
    bootBytes.replace(0, 6, gameId);
    assert(boot.write(bootBytes) == bootBytes.size());
    boot.close();
    QFile banner(folder + "/meta/opening.bnr");
    assert(banner.open(QIODevice::WriteOnly));
    QByteArray bytes(0x1960, '\0');
    bytes.replace(0, 4, "BNR1");
    for (int i = 0x20; i < 0x20 + 96 * 32 * 2; i += 2)
      qToBigEndian<quint16>(color, bytes.data() + i);
    assert(banner.write(bytes) == bytes.size());
  };
  makeCachedBanner("hp", "GAZE69", 0xfc00); // red: ignored because save is green
  makeCachedBanner("re4", "G4BE08", 0x801f); // blue: fallback
  const QMap<QString, QString> games{{"GAZE69", "Harry Potter"},
                                    {"G4BE08", "Resident Evil"},
                                    {"GTEE01", "No artwork"}};
  for (const auto mode : {ModManagerDialog::Mode::CheatCodes,
                          ModManagerDialog::Mode::TexturePacks}) {
    ModManagerDialog dialog(mode, temp.filePath("mods"), games, "GAZE69", temp.path());
    const auto *picker = dialog.findChild<QComboBox*>("modGameSelector");
    assert(picker && picker->count() == 3);
    const int hp = picker->findData("GAZE69");
    assert(hp >= 0 && !picker->itemIcon(hp).isNull());
    const QImage icon = picker->itemIcon(hp).pixmap(32, 32).toImage();
    assert(qGreen(icon.pixel(16, 16)) > 200 && qRed(icon.pixel(16, 16)) < 50);
    const int re = picker->findData("G4BE08");
    assert(re >= 0 && !picker->itemIcon(re).isNull());
    const QImage bnr = picker->itemIcon(re).pixmap(40, 40).toImage();
    assert(qBlue(bnr.pixel(bnr.width() / 2, bnr.height() / 2)) > 200 && qGreen(bnr.pixel(bnr.width() / 2, bnr.height() / 2)) < 50);
    assert(!picker->itemIcon(picker->findData("GTEE01")).isNull());
    assert(picker->property("shellChoiceSearchPlaceholder").toString() == "Search games…");
  }
  assert(ModManagerDialog::validateGecko("04000100 12345678\n", &why));
  assert(ModManagerDialog::validateGecko(
      "06000400 00000009\n11223344 55667788\n99000000 00000000", &why));
  assert(!ModManagerDialog::validateGecko("C2000100 00000001", &why));
  assert(!ModManagerDialog::validateGecko("04000100", &why));
  assert(!ModManagerDialog::validateGecko("06000400 00000009\n11223344 55667788", &why));

  QVector<ModManagerDialog::Cheat> cheats = {
      {"Enabled", "04000100 12345678", true},
      {"Disabled", "02000200 0000ABCD", false}};
  assert(ModManagerDialog::writeCheats(temp.path(), cheats, &why));
  QFile active(QDir(temp.path()).filePath("cheats.txt"));
  assert(active.open(QIODevice::ReadOnly));
  const auto runtimeCodes = active.readAll();
  assert(runtimeCodes.contains("04000100 12345678"));
  assert(!runtimeCodes.contains("02000200 0000ABCD"));
  assert(QFile::exists(QDir(temp.path()).filePath("cheats-gui.json")));
  const auto imported = ModManagerDialog::importGecko("$One\n04000100 12345678\n$Two\n02000200 0000ABCD\n");
  assert(imported.size() == 2);

  const QString external = QDir(temp.path()).filePath("pack");
  assert(QDir().mkpath(external));
  QVector<ModManagerDialog::Pack> packs = {{"First", external, true},
                                           {"Disabled", external, false}};
  assert(ModManagerDialog::writePacks(temp.path(), packs, &why));
  QFile manifest(QDir(temp.path()).filePath("texture-packs.txt"));
  assert(manifest.open(QIODevice::ReadOnly));
  const QByteArray paths = manifest.readAll();
  assert(paths.count(external.toUtf8()) == 1);
  packs[0].enabled = false;
  assert(ModManagerDialog::writePacks(temp.path(), packs, &why));
  manifest.close();
  assert(manifest.open(QIODevice::ReadOnly));
  assert(!manifest.readAll().contains(external.toUtf8()));
}
