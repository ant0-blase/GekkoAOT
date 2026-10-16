// SPDX-License-Identifier: GPL-3.0-or-later
// Save-serial rebinding adapted from Dolphin Emulator Project, Copyright 2008,
// GPL-2.0-or-later, used under GPL-3.0-or-later. See THIRD_PARTY_NOTICES.md.
// Interoperability references and supported container layouts:
// docs/memory-cards.md.
#include "MemoryCard.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringDecoder>
#include <QTimeZone>
#include <QUuid>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <utility>

static void initializeMemoryCardAssets() {
  static const bool ready = [] {
    Q_INIT_RESOURCE(memory_card_assets);
    return true;
  }();
  Q_UNUSED(ready);
}

namespace MemoryCards {
namespace {
bool fail(QString *error, const QString &text) {
  if (error)
    *error = text;
  return false;
}
quint16 u16(const QByteArray &b, qsizetype p) {
  return qFromBigEndian<quint16>(b.constData() + p);
}
quint32 u32(const QByteArray &b, qsizetype p) {
  return qFromBigEndian<quint32>(b.constData() + p);
}
void put16(QByteArray &b, qsizetype p, quint16 v) {
  qToBigEndian(v, b.data() + p);
}
void put32(QByteArray &b, qsizetype p, quint32 v) {
  qToBigEndian(v, b.data() + p);
}
QByteArray terminated(QByteArray b) {
  const auto end = b.indexOf('\0');
  if (end >= 0)
    b.truncate(end);
  return b;
}
QByteArray digest(const QByteArray &b) {
  return QCryptographicHash::hash(b, QCryptographicHash::Sha256);
}
QString text(const QByteArray &bytes, bool japanese) {
  initializeMemoryCardAssets();
  const auto b = terminated(bytes);
  if (japanese) {
    // Qt's optional ICU codecs are not present in every Windows/Android
    // build. The embedded JIS table keeps Japanese save names portable.
    static const QByteArray table = [] {
      QFile f(":/launcher/data/shift-jis-u16.bin");
      return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
    }();
    QString out;
    for (qsizetype i = 0; i < b.size(); ++i) {
      const auto c = quint8(b[i]);
      if (c < 0x80)
        out += QChar(c);
      else if (c >= 0xa1 && c <= 0xdf)
        out += QChar(0xff61 + c - 0xa1);
      else if (i + 1 < b.size()) {
        const unsigned index = (unsigned(c) << 8) | quint8(b[++i]);
        const auto value = table.size() == 131072 ? u16(table, index * 2) : 0;
        out += QChar(value ? value : 0xfffd);
      } else
        out += QChar::ReplacementCharacter;
    }
    return out.trimmed();
  }
  static constexpr char16_t cp1252[] = {
      0x20ac, 0xfffd, 0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
      0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0xfffd, 0x17d,  0xfffd,
      0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
      0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0xfffd, 0x17e,  0x178};
  QString out;
  for (auto c : b) {
    const auto v = quint8(c);
    out += QChar(v >= 0x80 && v < 0xa0 ? cp1252[v - 0x80] : v);
  }
  return out.trimmed();
}
std::pair<quint16, quint16> checksum(const QByteArray &b, int start, int end) {
  quint16 sum = 0, inverse = 0;
  for (int i = start; i < end; i += 2) {
    const auto v = u16(b, i);
    sum = quint16(sum + v);
    inverse = quint16(inverse + quint16(~v));
  }
  return {sum == 0xffff ? 0 : sum, inverse == 0xffff ? 0 : inverse};
}
void fix(QByteArray &b, int start, int end, int destination) {
  const auto [a, c] = checksum(b, start, end);
  put16(b, destination, a);
  put16(b, destination + 2, c);
}
bool validChecksum(const QByteArray &b, int start, int end, int destination) {
  const auto [a, c] = checksum(b, start, end);
  return a == u16(b, destination) && c == u16(b, destination + 2);
}
bool validSave(const Save &s) {
  return s.entry.size() == 64 && !terminated(s.entry.mid(8, 32)).isEmpty() &&
         s.entry.left(4) != QByteArray(4, '\xff') && s.data.size() > 0 &&
         s.data.size() % BlockSize == 0 && s.blocks() <= 2043 &&
         u16(s.entry, 0x38) == s.blocks();
}
bool read(const QString &path, QByteArray &bytes, QString *error) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return fail(error, f.errorString());
  if (f.size() < 64 || f.size() > 16 * 1024 * 1024 + BlockSize)
    return fail(error, "Unsupported or truncated memory card/save size.");
  bytes = f.readAll();
  return bytes.size() == f.size() ||
         fail(error, "Could not read the complete file.");
}
bool write(const QString &path, const QByteArray &bytes, QString *error) {
  QSaveFile f(path);
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() ||
      !f.commit())
    return fail(error, f.errorString());
  return true;
}
void swapSav(QByteArray &entry) {
  for (int p :
       {6, 0x2c, 0x2e, 0x30, 0x32, 0x34, 0x36, 0x38, 0x3a, 0x3c, 0x3e}) {
    const char c = entry[p];
    entry[p] = entry[p + 1];
    entry[p + 1] = c;
  }
}
QRgb rgb5a3(quint16 v) {
  if (v & 0x8000)
    return qRgba(((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31,
                 (v & 31) * 255 / 31, 255);
  return qRgba(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17,
               ((v >> 12) & 7) * 255 / 7);
}
QImage texture(const QByteArray &bytes, quint64 offset, int width, int height,
               bool indexed, quint64 palette = 0) {
  const quint64 size = quint64(width) * height * (indexed ? 1 : 2);
  if (offset > quint64(bytes.size()) || size > quint64(bytes.size()) - offset ||
      (indexed && (palette > quint64(bytes.size()) ||
                   512 > quint64(bytes.size()) - palette)))
    return {};
  QImage image(width, height, QImage::Format_ARGB32);
  int pos = int(offset);
  const int tileWidth = indexed ? 8 : 4;
  for (int ty = 0; ty < height; ty += 4)
    for (int tx = 0; tx < width; tx += tileWidth)
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < tileWidth; ++x) {
          const auto value =
              indexed ? u16(bytes, int(palette) + 2 * quint8(bytes[pos++]))
                      : u16(bytes, std::exchange(pos, pos + 2));
          image.setPixel(tx + x, ty + y, rgb5a3(value));
        }
  return image;
}
// Verify ownership as well as checksums: malformed chains must never be
// silently reallocated over another save, even when their checksums match.
bool filesystem(const QByteArray &image, int directory, int bat,
                QList<Save> &saves) {
  const int blocks = int(image.size() / BlockSize), d = directory * BlockSize,
            b = bat * BlockSize;
  QSet<int> owned;
  saves.clear();
  for (int row = 0; row < 127; ++row) {
    Save save;
    save.entry = image.mid(d + row * 64, 64);
    if (save.entry.left(4) == QByteArray(4, '\xff'))
      continue;
    const int count = u16(save.entry, 0x38);
    int block = u16(save.entry, 0x36);
    if (count < 1 || count > blocks - 5)
      return false;
    for (int i = 0; i < count; ++i) {
      if (block < 5 || block >= blocks || owned.contains(block))
        return false;
      owned.insert(block);
      save.data += image.mid(block * BlockSize, BlockSize);
      block = u16(image, b + 10 + 2 * (block - 5));
    }
    if (block != 0xffff || !validSave(save))
      return false;
    saves.append(std::move(save));
  }
  int free = 0;
  for (int block = 5; block < blocks; ++block) {
    const int next = u16(image, b + 10 + 2 * (block - 5));
    if (next == 0)
      ++free;
    if ((next != 0) != owned.contains(block))
      return false;
  }
  return free == u16(image, b + 6);
}
void bindSerial(Save &save, const QByteArray &header) {
  quint32 serial[2]{};
  for (int p = 0; p < 32; p += 4)
    serial[(p / 4) % 2] ^= u32(header, p);
  const auto name = terminated(save.entry.mid(8, 32));
  if (name == "f_zero.dat" && save.blocks() == 4) {
    put16(save.data, BlockSize + 0x66, quint16(serial[0] >> 16));
    put16(save.data, 3 * BlockSize + 0x1580, quint16(serial[1] >> 16));
    put16(save.data, BlockSize + 0x60, quint16(serial[0]));
    put16(save.data, BlockSize + 0x200, quint16(serial[1]));
    quint16 crc = 0xffff;
    for (int i = 2; i < save.data.size(); ++i) {
      crc ^= quint8(save.data[i]);
      for (int bit = 0; bit < 8; ++bit)
        crc = quint16((crc >> 1) ^ ((crc & 1) ? 0x8408 : 0));
    }
    put16(save.data, 0, quint16(~crc));
  } else if ((name == "PSO_SYSTEM" || name == "PSO3_SYSTEM") &&
             save.blocks() >= 2) {
    const int base = BlockSize;
    put32(save.data, base + 0x158, serial[0]);
    put32(save.data, base + 0x15c, serial[1]);
    quint32 crc = 0xdebb20e3;
    for (int i = 0x4c; i < 0x164 + (name == "PSO3_SYSTEM" ? 16 : 0); ++i) {
      crc ^= quint8(save.data[base + i]);
      for (int bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    put32(save.data, base + 0x48, crc ^ 0xffffffffu);
  }
}
} // namespace

QString Save::gameId() const { return QString::fromLatin1(entry.left(6)); }
QString Save::filename() const {
  return text(entry.mid(8, 32), entry.size() == 64 && entry[3] == 'J');
}
QString Save::title() const {
  const auto offset = u32(entry, 0x3c);
  return offset <= quint32(data.size()) && 64 <= quint32(data.size()) - offset
             ? text(data.mid(offset, 32), entry[3] == 'J')
             : QString{};
}
QString Save::description() const {
  const auto offset = u32(entry, 0x3c);
  return offset <= quint32(data.size()) && 64 <= quint32(data.size()) - offset
             ? text(data.mid(offset + 32, 32), entry[3] == 'J')
             : QString{};
}
QString Save::exportFilename() const {
  QString name = gameId() + "_" + filename();
  name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1f]"), "_");
  name = name.left(140);
  while (name.endsWith('.') || name.endsWith(' '))
    name.chop(1);
  return name;
}
QByteArray Save::identity() const {
  return entry.left(6) + terminated(entry.mid(8, 32));
}
QDateTime Save::modified() const {
  return QDateTime::fromSecsSinceEpoch(946684800ll + u32(entry, 0x28),
                                       QTimeZone::UTC);
}
bool Save::pingPong() const { return quint8(entry[7]) & 4; }
QImage Save::banner() const {
  const auto offset = u32(entry, 0x2c);
  const auto format = quint8(entry[7]) & 3;
  if (format != 1 && format != 2)
    return {};
  return texture(data, offset, 96, 32, format == 1, quint64(offset) + 96 * 32);
}
QList<IconFrame> Save::icons() const {
  quint64 offset = u32(entry, 0x2c);
  if (offset == 0xffffffff)
    return {};
  const auto bannerFormat = quint8(entry[7]) & 3;
  if (bannerFormat == 1)
    offset += 3584;
  else if (bannerFormat == 2)
    offset += 6144;
  const auto formats = u16(entry, 0x30), speeds = u16(entry, 0x32);
  if (!(formats & 3))
    return {};
  std::array<int, 8> type{}, speed{};
  std::array<quint64, 8> start{};
  int count = 0;
  bool shared = false;
  for (int i = 0; i < 8; ++i) {
    speed[i] = (speeds >> (i * 2)) & 3;
    if (!speed[i])
      break;
    type[i] = (formats >> (i * 2)) & 3;
    start[i] = offset;
    ++count;
    if (type[i] == 1) {
      shared = true;
      offset += 1024;
    } else if (type[i] == 2)
      offset += 2048;
    else if (type[i] == 3)
      offset += 1536;
  }
  const auto sharedPalette = offset;
  if (shared)
    offset += 512;
  if (offset > quint64(data.size()))
    return {};
  QList<IconFrame> result;
  for (int i = 0; i < count; ++i) {
    QImage image(32, 32, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int j = i; j < count; ++j)
      if (type[j]) {
        image = texture(data, start[j], 32, 32, type[j] != 2,
                        type[j] == 1 ? sharedPalette : start[j] + 1024);
        break;
      }
    result.append({image, qRound(speed[i] * 4000.0 / 60)});
  }
  return result;
}
bool Card::decodeSave(const QByteArray &bytes, Save &result, QString *error) {
  const int header = int(bytes.size() % BlockSize);
  Save save;
  if (header == 64)
    save.entry = bytes.left(64);
  else if (header == 0x150 && bytes.startsWith("GCSAVE")) {
    save.entry = bytes.mid(0x110, 64);
    put16(save.entry, 0x38, quint16((bytes.size() - header) / BlockSize));
  } else if (header == 0xc0 && bytes.startsWith("DATELGC_SAVE")) {
    save.entry = bytes.mid(0x80, 64);
    swapSav(save.entry);
  } else
    return fail(error, "This is not a supported GCI, GCS or SAV save file.");
  save.data = bytes.mid(header);
  if (!validSave(save))
    return fail(
        error,
        "The save header and data size do not match, or the file is damaged.");
  result = std::move(save);
  return true;
}
QByteArray Card::encodeSave(const Save &save, SaveFormat format) {
  if (!validSave(save))
    return {};
  QByteArray header;
  if (format == SaveFormat::GCI)
    header = save.entry;
  else if (format == SaveFormat::GCS) {
    header = QByteArray(0x150, '\0');
    header.replace(0, 6, "GCSAVE");
    auto entry = save.entry;
    put16(entry, 0x38, 1);
    header.replace(0x110, 64, entry);
  } else {
    header = QByteArray(0xc0, '\0');
    header.replace(0, 12, "DATELGC_SAVE");
    auto entry = save.entry;
    swapSav(entry);
    header.replace(0x80, 64, entry);
  }
  return header + save.data;
}
bool Card::readSave(const QString &path, Save &result, QString *error) {
  QByteArray b;
  return read(path, b, error) && decodeSave(b, result, error);
}
bool Card::writeSave(const QString &path, const Save &save, SaveFormat format,
                     QString *error) {
  const auto b = encodeSave(save, format);
  return !b.isEmpty() ? write(path, b, error)
                      : fail(error, "Invalid save data.");
}
bool Card::open(const QString &path, QString *error) {
  QByteArray bytes;
  if (!read(path, bytes, error))
    return false;
  Card card;
  card.m_path = QFileInfo(path).absoluteFilePath();
  card.m_diskHash = digest(bytes);
  const qsizetype size = bytes.size();
  int prefix = 0;
  auto cardSize = [](qsizetype value) {
    return value >= 512 * 1024 && value <= 16 * 1024 * 1024 &&
           (value & (value - 1)) == 0;
  };
  if (!cardSize(size) && cardSize(size - 64) &&
      u16(bytes, 64 + 0x22) * 16 == (size - 64) / BlockSize)
    prefix = 64;
  if (cardSize(size - prefix) &&
      u16(bytes, prefix + 0x22) * 16 == (size - prefix) / BlockSize) {
    card.m_prefix = bytes.left(prefix);
    card.m_image = bytes.mid(prefix);
    if (!validChecksum(card.m_image, 0, 0x1fc, 0x1fc))
      return fail(error, "The card header checksum is invalid. The original "
                         "file was left untouched.");
    QList<int> directories, bats;
    for (int d : {1, 2})
      if (validChecksum(card.m_image, d * BlockSize, (d + 1) * BlockSize - 4,
                        (d + 1) * BlockSize - 4))
        directories << d;
    for (int b : {3, 4})
      if (validChecksum(card.m_image, b * BlockSize + 4, (b + 1) * BlockSize,
                        b * BlockSize))
        bats << b;
    auto newest = [&](int a, int b, int offset) {
      return qint16(u16(card.m_image, a * BlockSize + offset) -
                    u16(card.m_image, b * BlockSize + offset)) > 0;
    };
    if (directories.size() == 2 && newest(2, 1, 0x1ffa))
      std::swap(directories[0], directories[1]);
    if (bats.size() == 2 && newest(4, 3, 4))
      std::swap(bats[0], bats[1]);
    bool found = false;
    for (int d : directories) {
      for (int b : bats)
        if (filesystem(card.m_image, d, b, card.m_saves)) {
          card.m_directoryCounter = u16(card.m_image, d * BlockSize + 0x1ffa);
          card.m_batCounter = u16(card.m_image, b * BlockSize + 4);
          found = true;
          break;
        }
      if (found)
        break;
    }
    if (!found)
      return fail(error, "No consistent directory/allocation-table pair was "
                         "found. The original card was left untouched.");
  } else {
    Save save;
    if (!decodeSave(bytes, save, error))
      return false;
    card.m_saves << save;
  }
  *this = std::move(card);
  return true;
}
bool Card::create(const QString &path, int usableBlocks, bool japanese,
                  QString *error) {
  if (!QList<int>{59, 123, 251, 507, 1019, 2043}.contains(usableBlocks))
    return fail(error, "Unsupported card capacity.");
  if (QFileInfo::exists(path))
    return fail(
        error,
        "A file already exists at this location. Choose a new card name.");
  Card card;
  card.m_path = QFileInfo(path).absoluteFilePath();
  card.m_image = QByteArray((usableBlocks + 5) * BlockSize, '\xff');
  const quint64 ticks =
      quint64(
          qMax<qint64>(0, QDateTime::currentSecsSinceEpoch() - 946684800ll)) *
      40500000ull;
  quint64 seed = ticks;
  for (int i = 0; i < 12; ++i) {
    seed = (seed * 0x41c64e6dull + 0x3039ull) >> 16;
    card.m_image[i] = char(seed);
    seed = ((seed * 0x41c64e6dull + 0x3039ull) >> 16) & 0x7fff;
  }
  qToBigEndian(ticks, card.m_image.data() + 0x0c);
  std::fill(card.m_image.begin() + 0x14, card.m_image.begin() + 0x20, 0);
  put16(card.m_image, 0x20, 0);
  put16(card.m_image, 0x22, quint16((usableBlocks + 5) / 16));
  put16(card.m_image, 0x24, japanese ? 1 : 0);
  fix(card.m_image, 0, 0x1fc, 0x1fc);
  card.rebuild();
  if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    return fail(error, "Could not create the memory card directory.");
  if (!card.commit(error))
    return false;
  *this = std::move(card);
  return true;
}
int Card::usedBlocks() const {
  int n = 0;
  for (const auto &s : m_saves)
    n += s.blocks();
  return n;
}
bool Card::japanese() const { return isImage() && u16(m_image, 0x24) == 1; }
void Card::rebuild() {
  const auto header = m_image.left(BlockSize);
  m_image.fill('\xff');
  m_image.replace(0, BlockSize, header);
  QByteArray directory(BlockSize, '\xff'), bat(BlockSize, '\0');
  int block = 5, row = 0;
  for (auto &save : m_saves) {
    put16(save.entry, 0x36, quint16(block));
    directory.replace(row++ * 64, 64, save.entry);
    for (int i = 0; i < save.blocks(); ++i, ++block) {
      m_image.replace(block * BlockSize, BlockSize,
                      save.data.mid(i * BlockSize, BlockSize));
      put16(bat, 10 + 2 * (block - 5),
            i + 1 == save.blocks() ? 0xffff : quint16(block + 1));
    }
  }
  put16(directory, 0x1ffa, ++m_directoryCounter);
  fix(directory, 0, 0x1ffc, 0x1ffc);
  put16(bat, 4, ++m_batCounter);
  put16(bat, 6, quint16(capacity() - usedBlocks()));
  put16(bat, 8, quint16(block - 1));
  fix(bat, 4, BlockSize, 0);
  for (int d : {1, 2})
    m_image.replace(d * BlockSize, BlockSize, directory);
  for (int b : {3, 4})
    m_image.replace(b * BlockSize, BlockSize, bat);
}
bool Card::importSaves(const QList<Save> &saves, bool replace, QString *error) {
  if (!isImage())
    return fail(error, "Open or create a memory card before importing saves.");
  auto staged = m_saves;
  QSet<QByteArray> incoming;
  for (auto save : saves) {
    if (!validSave(save))
      return fail(error, "An imported save is invalid.");
    if (incoming.contains(save.identity()))
      return fail(error, "The import contains the same save more than once.");
    incoming.insert(save.identity());
    auto existing =
        std::find_if(staged.begin(), staged.end(), [&](const Save &s) {
          return s.identity() == save.identity();
        });
    if (existing != staged.end()) {
      if (!replace)
        return fail(error,
                    "A save with this game ID and filename already exists.");
      staged.erase(existing);
    }
    bindSerial(save, m_image.left(BlockSize));
    staged << save;
  }
  int used = 0;
  for (const auto &save : staged)
    used += save.blocks();
  if (staged.size() > 127 || used > capacity())
    return fail(
        error,
        "There is not enough free space or directory entries on this card.");
  m_saves = std::move(staged);
  rebuild();
  return true;
}
bool Card::removeSaves(const QList<int> &indices, QString *error) {
  if (!isImage())
    return fail(error, "Open a memory card first.");
  for (int i : indices)
    if (i < 0 || i >= m_saves.size())
      return fail(error, "Invalid save selection.");
  auto sorted = indices;
  std::sort(sorted.begin(), sorted.end(), std::greater<int>());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  for (int i : sorted)
    m_saves.removeAt(i);
  rebuild();
  return true;
}
bool Card::commit(QString *error) {
  if (!isImage())
    return fail(error, "This is an individual save, not a card image.");
  QLockFile lock(m_path + ".gekkoaot.lock");
  if (!lock.tryLock(0))
    return fail(error, "This card is already being edited.");
  if (QFileInfo::exists(m_path)) {
    if (!QFileInfo(m_path).isWritable())
      return fail(
          error,
          "This card is read-only. Save a separate copy before editing it.");
    QByteArray original;
    if (!read(m_path, original, error))
      return false;
    if (m_diskHash.isEmpty() || digest(original) != m_diskHash)
      return fail(error, "The card changed on disk. Reopen it before editing "
                         "to preserve the newer data.");
    const QString backupDir = m_path + ".backups";
    if (!QDir().mkpath(backupDir))
      return fail(
          error,
          "Could not create a backup directory. No changes were written.");
    m_lastBackup = QDir(backupDir).filePath(
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-" +
        QUuid::createUuid().toString(QUuid::Id128).left(8) + "." +
        QFileInfo(m_path).suffix());
    if (!write(m_lastBackup, original, error))
      return false;
  } else if (!m_diskHash.isEmpty())
    return fail(error,
                "The original card disappeared. Reopen it before saving.");
  const auto bytes = m_prefix + m_image;
  if (!write(m_path, bytes, error))
    return false;
  m_diskHash = digest(bytes);
  return true;
}
bool Card::exportImage(const QString &path, QString *error) const {
  if (!isImage())
    return fail(error, "Open a card image first.");
  if (QFileInfo(path).absoluteFilePath() == m_path ||
      (!QFileInfo(path).canonicalFilePath().isEmpty() &&
       QFileInfo(path).canonicalFilePath() ==
           QFileInfo(m_path).canonicalFilePath()))
    return fail(error, "Choose a different destination for the card backup.");
  const auto suffix = QFileInfo(path).suffix().toLower();
  if (suffix != "raw" && suffix != "gcp" && suffix != "mci")
    return fail(error, "Choose a RAW, GCP or existing MCI card format.");
  if (suffix == "mci" && m_prefix.size() != 64)
    return fail(error, "Export this card as RAW or GCP. MCI headers are "
                       "preserved only for existing MCI images.");
  return write(path, (suffix == "mci" ? m_prefix : QByteArray{}) + m_image,
               error);
}
} // namespace MemoryCards
