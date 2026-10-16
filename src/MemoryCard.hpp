// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QList>
#include <QString>

namespace MemoryCards {
constexpr int BlockSize = 8192;
enum class SaveFormat { GCI, GCS, SAV };
struct IconFrame {
  QImage image;
  int milliseconds = 67;
};
struct Save {
  QByteArray entry, data;
  QString gameId() const;
  QString filename() const;
  QString title() const;
  QString description() const;
  QString exportFilename() const;
  QByteArray identity() const;
  int blocks() const { return int(data.size() / BlockSize); }
  QDateTime modified() const;
  QImage banner() const;
  QList<IconFrame> icons() const;
  bool pingPong() const;
};

// Filesystem operations are transactional: validate both filesystem copies,
// stage an edit in memory, then compare the source and atomically replace it.
// Existing images receive a byte-for-byte backup before any edit is committed.
class Card {
public:
  bool open(const QString &path, QString *error = nullptr);
  bool create(const QString &path, int usableBlocks = 251,
              bool japanese = false, QString *error = nullptr);
  bool importSaves(const QList<Save> &saves, bool replace,
                   QString *error = nullptr);
  bool removeSaves(const QList<int> &indices, QString *error = nullptr);
  bool commit(QString *error = nullptr);
  bool exportImage(const QString &path, QString *error = nullptr) const;
  static bool readSave(const QString &path, Save &result,
                       QString *error = nullptr);
  static bool writeSave(const QString &path, const Save &save,
                        SaveFormat format, QString *error = nullptr);
  static bool decodeSave(const QByteArray &bytes, Save &result,
                         QString *error = nullptr);
  static QByteArray encodeSave(const Save &save, SaveFormat format);
  const QList<Save> &saves() const { return m_saves; }
  QString path() const { return m_path; }
  QString lastBackup() const { return m_lastBackup; }
  bool isImage() const { return !m_image.isEmpty(); }
  int capacity() const {
    return isImage() ? int(m_image.size() / BlockSize) - 5 : 0;
  }
  int usedBlocks() const;
  int freeBlocks() const { return capacity() - usedBlocks(); }
  bool japanese() const;

private:
  QString m_path, m_lastBackup;
  QByteArray m_image, m_prefix, m_diskHash;
  QList<Save> m_saves;
  quint16 m_directoryCounter = 0, m_batCounter = 0;
  void rebuild();
};
} // namespace MemoryCards
