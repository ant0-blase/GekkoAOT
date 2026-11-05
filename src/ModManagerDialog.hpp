// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QMap>
#include <QString>
#include <QVector>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

// A shared Shell-style manager: the controls and file dialogs are the same as
// Memory Card Manager.  Only the data pane changes between the two modes.
class ModManagerDialog final : public QDialog {
  Q_OBJECT
public:
  enum class Mode { CheatCodes, TexturePacks };
  struct Cheat {
    QString name;
    QString code;
    bool enabled = true;
  };
  struct Pack {
    QString name;
    QString folder;
    bool enabled = true;
  };

  ModManagerDialog(Mode mode, const QString &modsRoot,
                   const QMap<QString, QString> &gameTitles,
                   const QString &selectedGameId, const QString &stateRoot,
                   QWidget *parent = nullptr);

  // Used by the headless regression tests as well as the GUI.
  static bool validateGecko(const QString &source, QString *error = nullptr);
  static QVector<Cheat> importGecko(const QString &source);
  static bool writeCheats(const QString &folder, const QVector<Cheat> &cheats,
                          QString *error = nullptr);
  static bool writePacks(const QString &folder, const QVector<Pack> &packs,
                         QString *error = nullptr);

private:
  void loadGame();
  bool persist();
  void rebuildList(int preferredRow = -1);
  void showDetails();
  void notify(const QString &message, bool error = false);
  void createEntry();
  void editEntry();
  void importEntry();
  void deleteEntry();
  void openFolder();
  void makeLocalPack();
  QString chooseFile(const QString &title, const QString &filter,
                     bool multiple, QStringList *chosen = nullptr);
  QString activeFolder() const;
  int selectedRow() const;

  Mode m_mode;
  QString m_modsRoot;
  QString m_gameId;
  QComboBox *m_games = nullptr;
  QLineEdit *m_search = nullptr;
  QListWidget *m_list = nullptr;
  QLabel *m_title = nullptr;
  QLabel *m_description = nullptr;
  QLabel *m_status = nullptr;
  QFrame *m_details = nullptr;
  QLabel *m_preview = nullptr;
  QPlainTextEdit *m_codePreview = nullptr;
  QPushButton *m_edit = nullptr;
  QPushButton *m_delete = nullptr;
  QVector<Cheat> m_cheats;
  QVector<Pack> m_packs;
  bool m_loading = false;
};
