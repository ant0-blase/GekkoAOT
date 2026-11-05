// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "MemoryCard.hpp"
#include <QDialog>
#include <QElapsedTimer>
#include <QMap>
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QProgressBar;
class MemoryCardManager final : public QDialog {
public:
  MemoryCardManager(const QString &stateRoot, const QString &currentUserDir,
                    const QString &activeUserDir,
                    const QMap<QString, QString> &titles,
                    QWidget *parent = nullptr);
  bool openPath(const QString &path);

private:
  QString m_stateRoot, m_currentUserDir, m_activeUserDir;
  QMap<QString, QString> m_titles;
  MemoryCards::Card m_card;
  QComboBox *m_cards;
  QLineEdit *m_search;
  QListWidget *m_list;
  QLabel *m_capacity, *m_status, *m_banner, *m_icon, *m_title, *m_description,
      *m_info;
  QProgressBar *m_usage;
  QPushButton *m_import, *m_export, *m_exportAll, *m_backup, *m_delete;
  QList<MemoryCards::IconFrame> m_frames;
  QElapsedTimer m_animation;
  int m_lastFrame = -1;
  bool locked() const;
  void discover();
  void refresh();
  void selectSave();
  void animateIcon();
  void filter();
  void openFile();
  void createCard();
  void importFiles();
  void exportSelected(bool all = false);
  void backupCard();
  void removeSelected();
  void message(const QString &text, bool error = false);
  QString choosePath(const QString &title, const QString &filters,
                     const QString &initial, bool save);
  bool confirm(const QString &title, const QString &text,
               const QString &accept);
};
