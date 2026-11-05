// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Prefer the 32x32 icon stored in a GameCube save (GCI/RAW). Only when
// there is no valid icon for the game, use its disc opening.bnr artwork from
// the existing metadata cache. Never use an unrelated library cover.
#include "MemoryCard.hpp"
#include "DiscMetadata.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>

namespace MemoryCardSaveIcons {
inline QImage fromSave(const MemoryCards::Save &save) {
  const auto frames = save.icons();
  for (const auto &frame : frames)
    if (!frame.image.isNull()) return frame.image;
  return {};
}

inline QImage fromCard(const MemoryCards::Card &card) {
  for (const auto &save : card.saves()) {
    const QImage icon = fromSave(save);
    if (!icon.isNull()) return icon;
  }
  return {};
}

inline QImage fromCardFile(const QString &path) {
  MemoryCards::Card card;
  return card.open(path) ? fromCard(card) : QImage{};
}

// Decode only disc banners for requested game IDs. The cache directory names
// are SHA-256s of ISO paths, so match using the disc boot.bin header instead.
// Never scan raw ISOs from this UI helper. This runs on selector construction.
// A card can be completely empty: its save list is not a reliable source of
// game identity. Game-specific cards are stored in user/<slug>/cards and the
// corresponding extracted disc is cached in cache/discs/<slug>.
// Do not require any save to exist before checking the associated disc banner.
inline QImage bannerForSlug(const QString &stateRoot, const QString &slug) {
  if (slug.isEmpty() || slug == "." || slug == ".." || slug.contains('/') ||
      slug.contains('\\')) return {};
  QFile banner(QDir(stateRoot).filePath("cache/discs/" + slug + "/meta/opening.bnr"));
  if (!banner.open(QIODevice::ReadOnly)) return {};
  return decodeGameCubeBanner(banner.readAll());
}

// Discover banners from BOTH caches used by GekkoAOT. The library cache is
// SHA-256 keyed, while the compiled/extracted disc cache is slug keyed.
// Match by disc header ID (not by human-readable title or directory order).
inline QMap<QString, QImage> bannersForGames(const QString &stateRoot,
                                            const QSet<QString> &wantedIds) {
  QMap<QString, QImage> banners;
  QSet<QString> remaining;
  for (const auto &id : wantedIds) remaining.insert(id.toUpper());
  if (remaining.isEmpty()) return banners;
  for (const char *cacheRoot : {"library/discs", "cache/discs"}) {
    const QDir caches(QDir(stateRoot).filePath(QString::fromLatin1(cacheRoot)));
    for (const auto &directory : caches.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                      QDir::Name)) {
      const QDir disc(directory.absoluteFilePath());
      QString gameId;
      QFile boot(disc.filePath("sys/boot.bin"));
      if (boot.open(QIODevice::ReadOnly)) gameId = QString::fromLatin1(boot.read(6)).toUpper();
      if (!remaining.contains(gameId)) {
        QFile idFile(disc.filePath("meta/disc-id.txt"));
        if (idFile.open(QIODevice::ReadOnly))
          gameId = QString::fromLatin1(idFile.readAll()).trimmed().toUpper();
      }
      if (!remaining.contains(gameId)) continue;
      QFile imageFile(disc.filePath("meta/opening.bnr"));
      if (!imageFile.open(QIODevice::ReadOnly)) continue;
      const QImage image = decodeGameCubeBanner(imageFile.readAll());
      if (image.isNull()) continue;
      banners.insert(gameId, image);
      remaining.remove(gameId);
      if (remaining.isEmpty()) return banners;
    }
  }
  return banners;
}

inline QImage fromCardOrBanner(const MemoryCards::Card &card,
                              const QMap<QString, QImage> &banners) {
  const QImage icon = fromCard(card);
  if (!icon.isNull()) return icon;
  for (const auto &save : card.saves()) {
    const QImage banner = banners.value(save.gameId().toUpper());
    if (!banner.isNull()) return banner;
  }
  return {};
}

// Called when the selector is built, never from search textChanged/paintEvent.
// A cached BNR is consulted only for game IDs missing usable save icons.
// Never substitute another game's artwork.
inline QMap<QString, QImage> forGames(const QString &stateRoot,
                                      const QSet<QString> &wantedIds) {
  QMap<QString, QImage> icons;
  QSet<QString> pending;
  for (const auto &gameId : wantedIds) pending.insert(gameId.toUpper());
  if (pending.isEmpty()) return icons;
  QDirIterator files(QDir(stateRoot).filePath("user"),
                     {"*.raw", "*.gcp", "*.mci"}, QDir::Files,
                     QDirIterator::Subdirectories);
  QStringList paths;
  while (files.hasNext()) {
    const auto path = files.next();
    if (QFileInfo(path).dir().dirName() == "cards") paths << path;
  }
  paths.sort(Qt::CaseInsensitive); // Deterministic when a game has multiple saves.
  for (const auto &path : paths) {
    MemoryCards::Card card;
    if (!card.open(path)) continue;
    for (const auto &save : card.saves()) {
      const auto id = save.gameId().toUpper();
      if (!pending.contains(id)) continue;
      const auto icon = fromSave(save);
      if (icon.isNull()) continue;
      icons.insert(id, icon);
      pending.remove(id);
      if (pending.isEmpty()) return icons;
    }
  }
  const auto banners = bannersForGames(stateRoot, pending);
  for (auto it = banners.cbegin(); it != banners.cend(); ++it)
    icons.insert(it.key(), it.value());
  return icons;
}
} // namespace MemoryCardSaveIcons
