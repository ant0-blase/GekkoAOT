// SPDX-License-Identifier: GPL-3.0-or-later
#include "ModManagerDialog.hpp"
#include "ShellStyle.hpp"
#include "MemoryCardSaveIcons.hpp"

#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QStringList>
#include <QtGlobal>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr auto kCheatsJson = "cheats-gui.json";
constexpr auto kPacksJson = "texture-packs-gui.json";
const QRegularExpression kGameId("^[A-Z0-9]{6}$");
const QRegularExpression kGeckoPair("^([0-9A-Fa-f]{8})\\s+([0-9A-Fa-f]{8})$");

bool saveAtomic(const QString &path, const QByteArray &bytes, QString *error) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    if (error) *error = "Cannot create the mod directory";
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
    if (error) *error = "Could not write " + path + ": " + file.errorString();
    return false;
  }
  return true;
}
QStringList codeLines(const QString &source) {
  QStringList lines;
  for (QString line : source.split('\n')) {
    const int comment = line.indexOf(QRegularExpression("[#;]"));
    if (comment >= 0) line.truncate(comment);
    line = line.trimmed();
    if (!line.isEmpty() && !line.startsWith('$') && !line.startsWith('[')) lines << line;
  }
  return lines;
}
QString canonicalFolder(const QString &path) {
  const QFileInfo info(path);
  const QString resolved = info.canonicalFilePath();
  return QDir::cleanPath(resolved.isEmpty() ? info.absoluteFilePath() : resolved);
}
QString nameForPack(const QString &path) {
  QString name = QFileInfo(path).fileName();
  if (name.isEmpty()) name = "Texture pack";
  return name;
}
QString readFile(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
}
} // namespace

bool ModManagerDialog::validateGecko(const QString &source, QString *error) {
  const QStringList lines = codeLines(source);
  if (lines.isEmpty()) {
    if (error) *error = "Enter at least one data-only Gecko code.";
    return false;
  }
  for (int i = 0; i < lines.size(); ++i) {
    auto match = kGeckoPair.match(lines[i]);
    if (!match.hasMatch()) {
      if (error) *error = QString("Line %1: expected two 8-digit hex words.").arg(i + 1);
      return false;
    }
    bool ok = false;
    const quint32 head = match.captured(1).toUInt(&ok, 16);
    const quint32 length = match.captured(2).toUInt(nullptr, 16);
    const quint32 type = head >> 24;
    if (type != 0x00 && type != 0x02 && type != 0x04 && type != 0x06) {
      if (error) *error = QString("Line %1: only Gecko 00/02/04/06 data codes are supported.").arg(i + 1);
      return false;
    }
    const quint32 size = type == 0x06 ? length : (type == 0x04 ? 4u : type == 0x02 ? 2u : 1u);
    const quint32 offset = head & 0x01ffffffu;
    if (!ok || (type == 0x06 && (length == 0 || length > 1024 * 1024)) ||
        offset >= 0x01800000u || size > 0x01800000u - offset) {
      if (error) *error = QString("Line %1: invalid or out-of-range MEM1 write.").arg(i + 1);
      return false;
    }
    if (type == 0x06) {
      const quint32 payloadLines = (length + 7u) / 8u;
      if (static_cast<quint32>(lines.size() - i - 1) < payloadLines) {
        if (error) *error = QString("Line %1: truncated Gecko 06 payload.").arg(i + 1);
        return false;
      }
      for (quint32 n = 0; n < payloadLines; ++n) {
        if (!kGeckoPair.match(lines[++i]).hasMatch()) {
          if (error) *error = QString("Line %1: invalid 06 payload pair.").arg(i + 1);
          return false;
        }
      }
    }
  }
  if (error) error->clear();
  return true;
}

QVector<ModManagerDialog::Cheat> ModManagerDialog::importGecko(const QString &source) {
  QVector<Cheat> results;
  Cheat current;
  current.name = "Imported codes";
  for (QString line : source.split('\n')) {
    line = line.trimmed();
    if (line.startsWith('$')) {
      if (!current.code.trimmed().isEmpty()) results << current;
      current = Cheat{line.mid(1).trimmed(), {}, true};
      if (current.name.isEmpty()) current.name = "Cheat";
    } else if (line.startsWith('#') || line.startsWith(';') || line.startsWith('[') || line.isEmpty()) {
      continue;
    } else {
      current.code += line + '\n';
    }
  }
  if (!current.code.trimmed().isEmpty()) results << current;
  return results;
}

bool ModManagerDialog::writeCheats(const QString &folder, const QVector<Cheat> &cheats,
                                   QString *error) {
  QJsonArray entries;
  QByteArray enabled;
  for (const auto &cheat : cheats) {
    QString validation;
    if (!validateGecko(cheat.code, &validation)) {
      if (error) *error = cheat.name + ": " + validation;
      return false;
    }
    entries.append(QJsonObject{{"name", cheat.name}, {"code", cheat.code},
                               {"enabled", cheat.enabled}});
    if (cheat.enabled) {
      QByteArray label = cheat.name.toUtf8();
      label.replace('\n', ' ');
      enabled.append('$');
      enabled.append(label);
      enabled.append('\n');
      for (const auto &line : codeLines(cheat.code)) enabled += line.toLatin1() + "\n";
    }
  }
  const auto json = QJsonDocument(QJsonObject{{"version", 1}, {"cheats", entries}}).toJson();
  // Write runtime payload first. If JSON fails, the runtime still has the new
  // valid code list rather than a stale enabled code with an invalid edit.
  if (!saveAtomic(QDir(folder).filePath("cheats.txt"), enabled, error)) return false;
  return saveAtomic(QDir(folder).filePath(kCheatsJson), json, error);
}

bool ModManagerDialog::writePacks(const QString &folder, const QVector<Pack> &packs,
                                  QString *error) {
  QJsonArray entries;
  QByteArray enabled("# GekkoAOT enabled texture directories v1\n");
  for (const auto &pack : packs) {
    if (pack.folder.contains('\n') || pack.folder.contains('\r')) {
      if (error) *error = "Invalid texture pack path";
      return false;
    }
    entries.append(QJsonObject{{"name", pack.name}, {"path", pack.folder},
                               {"enabled", pack.enabled}});
    if (pack.enabled) {
      if (!QFileInfo(pack.folder).isDir()) {
        if (error) *error = "Missing texture pack directory: " + pack.folder;
        return false;
      }
      enabled += QFileInfo(pack.folder).absoluteFilePath().toUtf8() + "\n";
    }
  }
  if (!saveAtomic(QDir(folder).filePath("texture-packs.txt"), enabled, error)) return false;
  return saveAtomic(QDir(folder).filePath(kPacksJson),
                    QJsonDocument(QJsonObject{{"version", 1}, {"packs", entries}}).toJson(), error);
}

ModManagerDialog::ModManagerDialog(Mode mode, const QString &modsRoot,
                                   const QMap<QString, QString> &gameTitles,
                                   const QString &selectedGameId,
                                   const QString &stateRoot, QWidget *parent)
    : QDialog(parent), m_mode(mode), m_modsRoot(modsRoot) {
  const bool cheats = mode == Mode::CheatCodes;
  setObjectName(cheats ? "cheatCodesManager" : "texturePacksManager");
  setWindowTitle(cheats ? "Cheat codes" : "Texture packs");
  resize(980, 650);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(22, 22, 22, 22);
  layout->setSpacing(18);
  auto *toolbar = new QHBoxLayout;
  toolbar->setSpacing(10);
  m_games = new QComboBox;
  m_games->setObjectName("modGameSelector");
  m_games->setProperty("shellCardChoices", true);
  m_games->setProperty("shellChoiceTitle", "Choose a game");
  m_games->setProperty("shellChoiceSearchPlaceholder", "Search games…");
  QSet<QString> wantedIds;
  for (auto it = gameTitles.cbegin(); it != gameTitles.cend(); ++it)
    if (kGameId.match(it.key()).hasMatch()) wantedIds.insert(it.key());
  const auto saveIcons = MemoryCardSaveIcons::forGames(stateRoot, wantedIds);
  for (auto it = gameTitles.cbegin(); it != gameTitles.cend(); ++it) {
    if (!kGameId.match(it.key()).hasMatch()) continue;
    const QImage image = saveIcons.value(it.key());
    m_games->addItem(image.isNull() ? shellIcon("memory-stick")
                                    : QIcon(QPixmap::fromImage(image)),
                     it.value() + " / " + it.key(), it.key());
  }
  m_games->setEnabled(m_games->count() > 0);
  toolbar->addWidget(m_games, 1);
  auto *folder = new QPushButton(shellIcon("folder-open"), "Open mod folder");
  folder->setObjectName("openModDirectory");
  toolbar->addWidget(folder);
  layout->addLayout(toolbar);

  auto *panes = new QHBoxLayout;
  panes->setSpacing(22);
  auto *browser = new QVBoxLayout;
  browser->setSpacing(10);
  m_search = new QLineEdit;
  m_search->setPlaceholderText(cheats ? "Search cheat codes…" : "Search texture packs…");
  m_search->addAction(shellIcon("search"), QLineEdit::LeadingPosition);
  browser->addWidget(m_search);
  m_list = new QListWidget;
  m_list->setObjectName(cheats ? "cheatCodesList" : "texturePacksList");
  m_list->setMinimumWidth(260);
  m_list->setStyleSheet(
      "QListWidget {background:rgba(255,255,255,3);border:1px solid rgba(255,255,255,18);"
      "border-radius:18px;outline:0;padding:6px;} "
      "QListWidget::item {padding:12px 10px;border:1px solid transparent;border-radius:12px;margin:2px;} "
      "QListWidget::item:selected {background:rgba(255,255,255,13);border-color:rgba(255,255,255,45);} "
      "QListWidget::item:hover {background:rgba(255,255,255,8);}");
  browser->addWidget(m_list, 1);
  panes->addLayout(browser, 5);

  auto *details = new QFrame;
  m_details = details;
  details->setObjectName("modDetails");
  details->setStyleSheet("QFrame#modDetails {background:rgba(255,255,255,3);"
                         "border:1px solid rgba(255,255,255,18);border-radius:18px;} "
                         "QLabel {background:transparent;}");
  auto *right = new QVBoxLayout(details);
  right->setContentsMargins(20, 22, 20, 22);
  right->setSpacing(14);
  m_preview = new QLabel;
  m_preview->setAlignment(Qt::AlignCenter);
  m_preview->setFixedHeight(108);
  m_preview->setObjectName("modTexturePreview");
  if (!cheats) right->addWidget(m_preview);
  m_title = new QLabel(cheats ? "Your Gecko cheats" : "Your texture packs");
  m_title->setWordWrap(true);
  m_title->setStyleSheet("font-size:21px;color:#eeeeee;");
  right->addWidget(m_title);
  m_description = new QLabel(cheats ? "Select a cheat to edit its code."
                                     : "Select a texture folder to inspect it.");
  m_description->setWordWrap(true);
  m_description->setStyleSheet("color:#adadad;");
  right->addWidget(m_description);
  m_codePreview = new QPlainTextEdit;
  m_codePreview->setObjectName("modCheatPreview");
  m_codePreview->setReadOnly(true);
  m_codePreview->setPlaceholderText("00/02/04/06 Gecko hex pairs");
  if (cheats) right->addWidget(m_codePreview, 1);
  else right->addStretch();
  auto *selection = new QHBoxLayout;
  m_edit = new QPushButton(shellIcon(cheats ? "file-text" : "folder-open"),
                           cheats ? "Edit" : "View folder");
  selection->addWidget(m_edit);
  m_delete = new QPushButton(shellIcon("trash"), "Remove");
  selection->addWidget(m_delete);
  right->addLayout(selection);
  panes->addWidget(details, 4);
  layout->addLayout(panes, 1);

  auto *actions = new QHBoxLayout;
  auto *add = new QPushButton(shellIcon(cheats ? "folder-plus" : "folder-plus"),
                              cheats ? "Add cheat…" : "Add pack folder…");
  add->setObjectName("addModEntry");
  actions->addWidget(add);
  auto *import = new QPushButton(shellIcon("file-input"),
                                 cheats ? "Import codes…" : "Import textures…");
  import->setObjectName("importModEntry");
  actions->addWidget(import);
  if (!cheats) {
    auto *local = new QPushButton(shellIcon("folder-plus"), "Local textures");
    actions->addWidget(local);
    connect(local, &QPushButton::clicked, this, &ModManagerDialog::makeLocalPack);
  }
  actions->addStretch();
  layout->addLayout(actions);
  m_status = new QLabel;
  m_status->setWordWrap(true);
  m_status->setMinimumHeight(24);
  layout->addWidget(m_status);

  connect(m_games, qOverload<int>(&QComboBox::activated), this,
          [this](int) { loadGame(); });
  connect(m_list, &QListWidget::currentRowChanged, this,
          [this] { showDetails(); });
  connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
    if (m_loading || !item) return;
    const int idx = item->data(Qt::UserRole).toInt();
    const bool enabled = item->checkState() == Qt::Checked;
    if (m_mode == Mode::CheatCodes && idx >= 0 && idx < m_cheats.size()) {
      const bool previous = m_cheats[idx].enabled;
      m_cheats[idx].enabled = enabled;
      if (!persist()) {
        m_cheats[idx].enabled = previous;
        QSignalBlocker block(m_list);
        item->setCheckState(previous ? Qt::Checked : Qt::Unchecked);
      }
    } else if (m_mode == Mode::TexturePacks && idx >= 0 && idx < m_packs.size()) {
      const bool previous = m_packs[idx].enabled;
      m_packs[idx].enabled = enabled;
      if (!persist()) {
        m_packs[idx].enabled = previous;
        QSignalBlocker block(m_list);
        item->setCheckState(previous ? Qt::Checked : Qt::Unchecked);
      }
    }
    showDetails();
  });
  connect(m_search, &QLineEdit::textChanged, this, [this] {
    for (int i = 0; i < m_list->count(); ++i)
      m_list->item(i)->setHidden(!m_list->item(i)->text().contains(m_search->text(), Qt::CaseInsensitive));
  });
  connect(add, &QPushButton::clicked, this, &ModManagerDialog::createEntry);
  connect(import, &QPushButton::clicked, this, &ModManagerDialog::importEntry);
  connect(m_edit, &QPushButton::clicked, this, &ModManagerDialog::editEntry);
  connect(m_delete, &QPushButton::clicked, this, &ModManagerDialog::deleteEntry);
  connect(folder, &QPushButton::clicked, this, &ModManagerDialog::openFolder);

  int selected = m_games->findData(selectedGameId.toUpper());
  if (selected < 0) selected = 0;
  if (m_games->count()) m_games->setCurrentIndex(selected);
  loadGame();
}

QString ModManagerDialog::activeFolder() const {
  return m_gameId.isEmpty() ? QString{} : QDir(m_modsRoot).filePath(m_gameId);
}
int ModManagerDialog::selectedRow() const {
  const auto *item = m_list->currentItem();
  return item ? item->data(Qt::UserRole).toInt() : -1;
}
void ModManagerDialog::notify(const QString &message, bool error) {
  m_status->setText(message);
  m_status->setStyleSheet(error ? "color:#ffaaaa;" : "color:#aaaaaa;");
}
void ModManagerDialog::loadGame() {
  m_gameId = m_games->currentData().toString();
  m_cheats.clear();
  m_packs.clear();
  if (!m_gameId.isEmpty()) {
    const QDir folder(activeFolder());
    if (m_mode == Mode::CheatCodes) {
      QFile jsonFile(folder.filePath(kCheatsJson));
      if (jsonFile.open(QIODevice::ReadOnly)) {
        const auto json = QJsonDocument::fromJson(jsonFile.readAll()).object().value("cheats").toArray();
        for (const auto &value : json) {
          const auto object = value.toObject();
          Cheat cheat{object.value("name").toString(), object.value("code").toString(),
                      object.value("enabled").toBool(true)};
          if (!cheat.code.trimmed().isEmpty()) m_cheats << cheat;
        }
      } else {
        m_cheats = importGecko(readFile(folder.filePath("cheats.txt")));
      }
    } else {
      QFile jsonFile(folder.filePath(kPacksJson));
      if (jsonFile.open(QIODevice::ReadOnly)) {
        const auto json = QJsonDocument::fromJson(jsonFile.readAll()).object().value("packs").toArray();
        for (const auto &value : json) {
          const auto object = value.toObject();
          Pack pack{object.value("name").toString(), object.value("path").toString(),
                    object.value("enabled").toBool(true)};
          if (!pack.folder.isEmpty()) m_packs << pack;
        }
      } else if (QFileInfo(folder.filePath("textures")).isDir()) {
        m_packs << Pack{"Local textures", canonicalFolder(folder.filePath("textures")), true};
      }
    }
  }
  m_search->clear();
  rebuildList();
  notify(m_gameId.isEmpty() ? "Add a game to your library to manage mods."
                            : "RAM cheats update live while playing; instruction patches need a relaunch.");
}

void ModManagerDialog::rebuildList(int preferredRow) {
  m_loading = true;
  QSignalBlocker block(m_list);
  m_list->clear();
  const int count = m_mode == Mode::CheatCodes ? m_cheats.size() : m_packs.size();
  for (int i = 0; i < count; ++i) {
    const QString name = m_mode == Mode::CheatCodes ? m_cheats[i].name : m_packs[i].name;
    const bool enabled = m_mode == Mode::CheatCodes ? m_cheats[i].enabled : m_packs[i].enabled;
    auto *item = new QListWidgetItem(shellIcon(m_mode == Mode::CheatCodes ? "file-text" : "library"), name);
    item->setData(Qt::UserRole, i);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    m_list->addItem(item);
  }
  m_loading = false;
  if (count > 0) m_list->setCurrentRow(qBound(0, preferredRow < 0 ? 0 : preferredRow, count - 1));
  showDetails();
}

void ModManagerDialog::showDetails() {
  const int idx = selectedRow();
  const bool selected = idx >= 0 &&
      (m_mode == Mode::CheatCodes ? idx < m_cheats.size() : idx < m_packs.size());
  m_edit->setEnabled(selected);
  m_delete->setEnabled(selected);
  if (!selected) {
    m_title->setText(m_mode == Mode::CheatCodes ? "Your Gecko cheats" : "Your texture packs");
    m_description->setText("Add an entry to get started.");
    m_codePreview->clear();
    m_preview->clear();
    return;
  }
  if (m_mode == Mode::CheatCodes) {
    const auto &cheat = m_cheats[idx];
    m_title->setText(cheat.name);
    m_description->setText(cheat.enabled ? "Enabled (RAM live; AOT instruction patches on relaunch)" : "Disabled");
    m_codePreview->setPlainText(cheat.code);
  } else {
    const auto &pack = m_packs[idx];
    m_title->setText(pack.name);
    QDirIterator files(pack.folder, {"*.png", "*.PNG", "*.dds", "*.DDS"}, QDir::Files,
                       QDirIterator::Subdirectories);
    QString firstPng;
    int count = 0;
    while (files.hasNext() && count < 512) {
      const QString path = files.next();
      if (firstPng.isEmpty() && QFileInfo(path).suffix().compare("png", Qt::CaseInsensitive) == 0)
        firstPng = path;
      ++count;
    }
    m_description->setText(QString("%1\n%2%3 texture files")
                           .arg(QDir::toNativeSeparators(pack.folder))
                           .arg(QFileInfo(pack.folder).isDir() ? "" : "Folder missing — ")
                           .arg(count >= 512 ? "At least 512" : QString::number(count)));
    m_preview->setPixmap(QPixmap{});
    m_preview->setText("PNG / DDS");
    if (!firstPng.isEmpty()) {
      QImageReader image(firstPng);
      image.setAutoTransform(true);
      image.setScaledSize({180, 100});
      const auto preview = image.read();
      if (!preview.isNull()) m_preview->setPixmap(QPixmap::fromImage(preview));
    }
  }
  // Same reveal animation as the other Shell settings cards, including
  // the existing reduced-motion policy in ShellStyle.
  if (isVisible() && m_details) shellReveal(m_details);
}

bool ModManagerDialog::persist() {
  QString error;
  const bool saved = m_mode == Mode::CheatCodes
                         ? writeCheats(activeFolder(), m_cheats, &error)
                         : writePacks(activeFolder(), m_packs, &error);
  notify(saved ? (m_mode == Mode::CheatCodes
               ? "Saved. RAM cheats reload automatically during gameplay; PPC instruction patches require AOT recompilation on relaunch."
               : "Saved. Texture packs are applied at the next game launch.")
               : error, !saved);
  return saved;
}

QString ModManagerDialog::chooseFile(const QString &title, const QString &filter,
                                     bool multiple, QStringList *chosen) {
  QFileDialog dialog(this, title, activeFolder());
  dialog.setOption(QFileDialog::DontUseNativeDialog);
  dialog.setNameFilters(filter.split(";;"));
  dialog.setFileMode(multiple ? QFileDialog::ExistingFiles : QFileDialog::ExistingFile);
  presentShellDialog(dialog, title, shellTVMode(this));
  if (execShellDialog(dialog) != QDialog::Accepted) return {};
  if (chosen) *chosen = dialog.selectedFiles();
  return dialog.selectedFiles().value(0);
}

void ModManagerDialog::createEntry() {
  if (m_gameId.isEmpty()) return;
  if (m_mode == Mode::TexturePacks) {
    const auto paths = shellChooseFiles(this, "Add texture pack folder", activeFolder(), false, true);
    if (paths.isEmpty()) return;
    const QString path = canonicalFolder(paths.first());
    if (!QFileInfo(path).isDir()) return;
    for (auto &pack : m_packs) {
      if (canonicalFolder(pack.folder) == path) {
        pack.enabled = true;
        persist(); rebuildList(); return;
      }
    }
    m_packs << Pack{nameForPack(path), path, true};
    if (persist()) rebuildList(m_packs.size() - 1);
    return;
  }
  Cheat cheat{"New cheat", {}, true};
  QDialog editor(this);
  editor.setObjectName("cheatEditor");
  auto *layout = new QVBoxLayout(&editor);
  auto *name = new QLineEdit(cheat.name);
  name->setObjectName("cheatName");
  layout->addWidget(new QLabel("Name")); layout->addWidget(name);
  auto *code = new QPlainTextEdit;
  code->setObjectName("cheatCodeInput");
  code->setPlaceholderText("04000100 12345678\n02000200 0000ABCD");
  code->setMinimumHeight(210);
  layout->addWidget(new QLabel("Gecko codes (00/02/04/06 data writes only)"));
  layout->addWidget(code);
  auto *validationLabel = new QLabel;
  validationLabel->setWordWrap(true);
  validationLabel->setStyleSheet("color:#ffaaaa;");
  layout->addWidget(validationLabel);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &editor, [&] {
    QString error;
    if (!validateGecko(code->toPlainText(), &error)) { validationLabel->setText(error); return; }
    cheat.name = name->text().trimmed().isEmpty() ? "Cheat" : name->text().trimmed();
    cheat.code = code->toPlainText(); editor.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
  presentShellDialog(editor, "Add cheat", shellTVMode(this));
  if (execShellDialog(editor) != QDialog::Accepted) return;
  m_cheats << cheat;
  if (persist()) rebuildList(m_cheats.size() - 1);
}

void ModManagerDialog::editEntry() {
  const int idx = selectedRow();
  if (idx < 0) return;
  if (m_mode == Mode::TexturePacks) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_packs[idx].folder));
    return;
  }
  Cheat cheat = m_cheats[idx];
  QDialog editor(this);
  editor.setObjectName("cheatEditor");
  auto *layout = new QVBoxLayout(&editor);
  auto *name = new QLineEdit(cheat.name);
  name->setObjectName("cheatName");
  layout->addWidget(new QLabel("Name")); layout->addWidget(name);
  auto *code = new QPlainTextEdit(cheat.code);
  code->setObjectName("cheatCodeInput");
  code->setMinimumHeight(220);
  layout->addWidget(new QLabel("Gecko codes")); layout->addWidget(code);
  auto *validationLabel = new QLabel;
  validationLabel->setWordWrap(true);
  validationLabel->setStyleSheet("color:#ffaaaa;");
  layout->addWidget(validationLabel);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &editor, [&] {
    QString error;
    if (!validateGecko(code->toPlainText(), &error)) { validationLabel->setText(error); return; }
    cheat.name = name->text().trimmed().isEmpty() ? "Cheat" : name->text().trimmed();
    cheat.code = code->toPlainText(); editor.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &editor, &QDialog::reject);
  presentShellDialog(editor, "Edit cheat", shellTVMode(this));
  if (execShellDialog(editor) == QDialog::Accepted) {
    m_cheats[idx] = cheat;
    if (persist()) rebuildList(idx);
  }
}

void ModManagerDialog::importEntry() {
  if (m_gameId.isEmpty()) return;
  if (m_mode == Mode::CheatCodes) {
    const QString path = chooseFile("Import Gecko codes", "Text files (*.txt);;All files (*)", false);
    if (path.isEmpty()) return;
    const QString source = readFile(path);
    QVector<Cheat> parsed = importGecko(source);
    if (parsed.isEmpty()) { notify("No code found in the selected file.", true); return; }
    for (const auto &cheat : parsed) {
      QString error;
      if (!validateGecko(cheat.code, &error)) {
        notify("Import rejected (" + cheat.name + "): " + error, true); return;
      }
    }
    m_cheats += parsed;
    if (persist()) rebuildList(m_cheats.size() - 1);
  } else {
    QStringList chosen;
    if (chooseFile("Import PNG/DDS textures", "Textures (*.png *.dds);;All files (*)", true, &chosen).isEmpty())
      return;
    const QString folder = QDir(activeFolder()).filePath("textures");
    if (!QDir().mkpath(folder)) { notify("Cannot create local textures directory", true); return; }
    int imported = 0;
    for (const auto &path : chosen) {
      const QString filename = QFileInfo(path).fileName();
      if (!filename.startsWith("tex1_", Qt::CaseInsensitive)) continue;
      const QString destination = QDir(folder).filePath(filename);
      if (QFileInfo::exists(destination)) continue; // Never overwrite an existing mod.
      if (QFile::copy(path, destination)) ++imported;
    }
    if (!imported) { notify("No new tex1_ PNG/DDS files imported (existing files are not overwritten).", true); return; }
    const QString canonical = canonicalFolder(folder);
    bool found = false;
    for (auto &pack : m_packs) if (canonicalFolder(pack.folder) == canonical) {
      pack.enabled = true; found = true;
    }
    if (!found) m_packs << Pack{"Local textures", canonical, true};
    if (persist()) rebuildList();
    notify(QString("Imported %1 textures. Restart the game to apply.").arg(imported));
  }
}

void ModManagerDialog::deleteEntry() {
  const int idx = selectedRow();
  if (idx < 0) return;
  QDialog dialog(this);
  auto *layout = new QVBoxLayout(&dialog);
  auto *text = new QLabel(m_mode == Mode::CheatCodes
      ? "Remove this cheat from the list?" : "Remove this pack from the list? Its files will not be deleted.");
  text->setWordWrap(true);
  layout->addWidget(text);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  buttons->addButton("Remove", QDialogButtonBox::AcceptRole);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  presentShellDialog(dialog, "Confirm removal", shellTVMode(this));
  if (execShellDialog(dialog) != QDialog::Accepted) return;
  if (m_mode == Mode::CheatCodes) m_cheats.removeAt(idx);
  else m_packs.removeAt(idx);
  if (persist()) rebuildList(qMax(0, idx - 1));
}

void ModManagerDialog::openFolder() {
  if (m_gameId.isEmpty()) return;
  if (!QDir().mkpath(activeFolder())) { notify("Cannot create mods folder", true); return; }
  QDesktopServices::openUrl(QUrl::fromLocalFile(activeFolder()));
}
void ModManagerDialog::makeLocalPack() {
  if (m_gameId.isEmpty()) return;
  const QString path = QDir(activeFolder()).filePath("textures");
  if (!QDir().mkpath(path)) { notify("Cannot create local textures folder", true); return; }
  const QString canonical = canonicalFolder(path);
  for (const auto &pack : m_packs) if (canonicalFolder(pack.folder) == canonical) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(path)); return;
  }
  m_packs << Pack{"Local textures", canonical, true};
  if (persist()) rebuildList(m_packs.size() - 1);
  QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
