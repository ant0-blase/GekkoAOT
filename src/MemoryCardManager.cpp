// SPDX-License-Identifier: GPL-3.0-or-later
#include "MemoryCardManager.hpp"
#include "ShellStyle.hpp"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {
const QString CardFilters = "Memory cards (*.raw *.gcp *.mci);;Individual "
                            "saves (*.gci *.gcs *.sav);;All files (*)";
QString displayPath(const QString &path) {
  QFileInfo file(path);
  QDir cards = file.dir();
  cards.cdUp();
  QString title = cards.dirName();
  title.replace('-', ' ');
  return title + " / " + file.fileName();
}
} // namespace
MemoryCardManager::MemoryCardManager(const QString &stateRoot,
                                     const QString &currentUserDir,
                                     const QString &activeUserDir,
                                     const QMap<QString, QString> &titles,
                                     QWidget *parent)
    : QDialog(parent), m_stateRoot(stateRoot), m_currentUserDir(currentUserDir),
      m_activeUserDir(activeUserDir), m_titles(titles) {
  setObjectName("memoryCardManager");
  setWindowTitle("Memory cards");
  resize(980, 650);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(22, 22, 22, 22);
  layout->setSpacing(18);
  auto *toolbar = new QHBoxLayout;
  toolbar->setSpacing(10);
  m_cards = new QComboBox;
  m_cards->setProperty("shellCardChoices", true);
  m_cards->setProperty("shellChoiceTitle", "Choose a memory card");
  m_cards->setObjectName("memoryCardFiles");
  m_cards->setMinimumContentsLength(15);
  m_cards->setSizeAdjustPolicy(
      QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_cards->setToolTip("Cards found in your GekkoAOT saves");
  toolbar->addWidget(m_cards, 1);
  auto *open = new QPushButton(shellIcon("folder-open"), "Open…");
  open->setObjectName("openMemoryCard");
  toolbar->addWidget(open);
  auto *create = new QPushButton(shellIcon("folder-plus"), "New card");
  create->setObjectName("createMemoryCard");
  toolbar->addWidget(create);
  layout->addLayout(toolbar);
  auto *summary = new QHBoxLayout;
  m_capacity = new QLabel;
  m_capacity->setObjectName("memoryCardCapacity");
  summary->addWidget(m_capacity, 1);
  m_usage = new QProgressBar;
  m_usage->setFixedWidth(150);
  m_usage->setFixedHeight(5);
  m_usage->setTextVisible(false);
  m_usage->setStyleSheet(
      "QProgressBar {border:none;background:#363636;border-radius:2px;} "
      "QProgressBar::chunk {background:#bfbfbf;border-radius:2px;}");
  summary->addWidget(m_usage);
  layout->addLayout(summary);
  auto *panes = new QHBoxLayout;
  panes->setSpacing(22);
  auto *browser = new QVBoxLayout;
  browser->setSpacing(10);
  m_search = new QLineEdit;
  m_search->setPlaceholderText("Search saves…");
  m_search->setObjectName("memoryCardSearch");
  m_search->addAction(shellIcon("search"), QLineEdit::LeadingPosition);
  browser->addWidget(m_search);
  m_list = new QListWidget;
  m_list->setObjectName("memoryCardSaves");
  m_list->setIconSize({48, 48});
  m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_list->setMinimumWidth(240);
  m_list->setTextElideMode(Qt::ElideRight);
  m_list->setStyleSheet(
      "QListWidget {background:rgba(255,255,255,3);border:1px solid "
      "rgba(255,255,255,18);border-radius:18px;outline:0;padding:6px;} "
      "QListWidget::item {padding:12px 10px;border:1px solid "
      "transparent;border-radius:12px;margin:2px;} QListWidget::item:selected "
      "{background:rgba(255,255,255,13);border-color:rgba(255,255,255,45);} "
      "QListWidget::item:hover {background:rgba(255,255,255,8);}");
  browser->addWidget(m_list, 1);
  panes->addLayout(browser, 5);
  auto *details = new QFrame;
  details->setObjectName("memoryCardDetails");
  details->setStyleSheet(
      "QFrame#memoryCardDetails {background:rgba(255,255,255,3);border:1px "
      "solid rgba(255,255,255,18);border-radius:18px;} QLabel "
      "{background:transparent;}");
  auto *preview = new QVBoxLayout(details);
  preview->setContentsMargins(20, 22, 20, 22);
  preview->setSpacing(14);
  auto *art = new QHBoxLayout;
  m_icon = new QLabel;
  m_icon->setFixedSize(64, 64);
  m_icon->setAlignment(Qt::AlignCenter);
  m_banner = new QLabel;
  m_banner->setFixedSize(192, 64);
  m_banner->setAlignment(Qt::AlignCenter);
  art->addWidget(m_icon);
  art->addStretch();
  art->addWidget(m_banner);
  preview->addLayout(art);
  m_title = new QLabel("Your saves, in one place");
  m_title->setObjectName("memorySaveTitle");
  m_title->setWordWrap(true);
  m_title->setStyleSheet("font-size:21px;color:#eeeeee;");
  preview->addWidget(m_title);
  m_description = new QLabel("Open a card or create a new one to get started.");
  m_description->setWordWrap(true);
  m_description->setStyleSheet("color:#adadad;");
  preview->addWidget(m_description);
  m_info = new QLabel;
  m_info->setObjectName("memorySaveInfo");
  m_info->setWordWrap(true);
  m_info->setStyleSheet("color:#adadad;line-height:1.6;");
  preview->addWidget(m_info);
  preview->addStretch();
  auto *selection = new QHBoxLayout;
  m_export = new QPushButton(shellIcon("file-output"), "Export");
  m_export->setObjectName("exportMemorySave");
  selection->addWidget(m_export);
  m_delete = new QPushButton(shellIcon("trash"), "Delete");
  m_delete->setObjectName("deleteMemorySave");
  selection->addWidget(m_delete);
  preview->addLayout(selection);
  panes->addWidget(details, 4);
  layout->addLayout(panes, 1);
  auto *actions = new QHBoxLayout;
  m_import = new QPushButton(shellIcon("file-input"), "Import saves…");
  m_import->setObjectName("importMemorySaves");
  actions->addWidget(m_import);
  actions->addStretch();
  m_exportAll = new QPushButton("Export all…");
  actions->addWidget(m_exportAll);
  m_backup = new QPushButton("Back up card…");
  actions->addWidget(m_backup);
  layout->addLayout(actions);
  m_status = new QLabel;
  m_status->setObjectName("memoryCardStatus");
  m_status->setWordWrap(true);
  m_status->setMinimumHeight(22);
  layout->addWidget(m_status);
  connect(open, &QPushButton::clicked, this, &MemoryCardManager::openFile);
  connect(create, &QPushButton::clicked, this, &MemoryCardManager::createCard);
  connect(m_cards, qOverload<int>(&QComboBox::activated), this,
          [this](int i) { openPath(m_cards->itemData(i).toString()); });
  connect(m_list, &QListWidget::currentRowChanged, this,
          [this] { selectSave(); });
  connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
    m_export->setEnabled(!m_list->selectedItems().isEmpty());
    m_delete->setEnabled(m_card.isImage() && !locked() &&
                         !m_list->selectedItems().isEmpty());
  });
  connect(m_search, &QLineEdit::textChanged, this, [this] { filter(); });
  connect(m_import, &QPushButton::clicked, this,
          &MemoryCardManager::importFiles);
  connect(m_export, &QPushButton::clicked, this, [this] { exportSelected(); });
  connect(m_exportAll, &QPushButton::clicked, this,
          [this] { exportSelected(true); });
  connect(m_backup, &QPushButton::clicked, this,
          &MemoryCardManager::backupCard);
  connect(m_delete, &QPushButton::clicked, this,
          &MemoryCardManager::removeSelected);
  auto *clock = new QTimer(this);
  clock->setInterval(34);
  connect(clock, &QTimer::timeout, this, &MemoryCardManager::animateIcon);
  clock->start();
  discover();
  const QString preferred =
      QDir(currentUserDir).filePath("cards/MemoryCardA.raw");
  if (QFileInfo::exists(preferred))
    openPath(preferred);
  else if (m_cards->count())
    openPath(m_cards->itemData(0).toString());
  else
    refresh();
}
bool MemoryCardManager::locked() const {
  if (m_activeUserDir.isEmpty() || m_card.path().isEmpty())
    return false;
  QDir directory(QDir(m_activeUserDir).filePath("cards"));
  QString active = directory.canonicalPath();
  if (active.isEmpty())
    active = directory.absolutePath();
  const QFileInfo file(m_card.path());
  QString path = file.canonicalFilePath();
  if (path.isEmpty())
    path = file.absoluteFilePath();
#ifdef Q_OS_WIN
  constexpr auto sensitivity = Qt::CaseInsensitive;
#else
  constexpr auto sensitivity = Qt::CaseSensitive;
#endif
  return QDir::fromNativeSeparators(path).startsWith(
      QDir::fromNativeSeparators(active) + "/", sensitivity);
}
void MemoryCardManager::discover() {
  QSignalBlocker block(m_cards);
  const auto selected = m_cards->currentData().toString();
  m_cards->clear();
  QDirIterator paths(QDir(m_stateRoot).filePath("user"),
                     {"*.raw", "*.gcp", "*.mci"}, QDir::Files,
                     QDirIterator::Subdirectories);
  while (paths.hasNext()) {
    const auto path = paths.next();
    if (QFileInfo(path).dir().dirName() == "cards") {
      QDir folder = QFileInfo(path).dir();
      folder.cdUp();
      QString fallback = folder.dirName();
      fallback.replace('-', ' ');
      m_cards->addItem(m_titles.value(folder.dirName(), fallback) + " / " +
                           QFileInfo(path).fileName(),
                       path);
    }
  }
  m_cards->model()->sort(0);
  if (!selected.isEmpty()) {
    const int i = m_cards->findData(selected);
    if (i >= 0)
      m_cards->setCurrentIndex(i);
  }
}
void MemoryCardManager::message(const QString &text, bool error) {
  m_status->setText(text);
  m_status->setStyleSheet(error ? "color:#e8b8b1;" : "color:#a7a7a7;");
}
bool MemoryCardManager::openPath(const QString &path) {
  QString error;
  MemoryCards::Card opened;
  if (!opened.open(path, &error)) {
    message(error, true);
    return false;
  }
  m_card = std::move(opened);
  int index = m_cards->findData(m_card.path());
  if (index < 0) {
    m_cards->addItem(displayPath(path), m_card.path());
    index = m_cards->count() - 1;
  }
  m_cards->setCurrentIndex(index);
  m_search->clear();
  refresh();
  message(locked() ? "This card is in use by the running game. Import and "
                     "delete will be available after the game stops."
                   : QFileInfo(path).absoluteFilePath());
  return true;
}
void MemoryCardManager::refresh() {
  m_list->clear();
  int row = 0;
  for (const auto &save : m_card.saves()) {
    const auto title = m_titles.value(
        save.gameId(), save.title().isEmpty() ? save.filename() : save.title());
    auto *item =
        new QListWidgetItem(title + "\n" +
                                QString("%1 · %2 block%3")
                                    .arg(save.gameId())
                                    .arg(save.blocks())
                                    .arg(save.blocks() == 1 ? "" : "s"),
                            m_list);
    item->setData(Qt::UserRole, row++);
    item->setSizeHint({0, 78});
    const auto frames = save.icons();
    item->setIcon(frames.isEmpty()
                      ? shellIcon("memory-stick")
                      : QIcon(QPixmap::fromImage(frames.first().image)));
    item->setToolTip(save.filename() + "\n" + save.description());
  }
  m_import->setEnabled(m_card.isImage() && !locked());
  m_exportAll->setEnabled(!m_card.saves().isEmpty());
  m_backup->setEnabled(m_card.isImage());
  m_delete->setEnabled(false);
  m_export->setEnabled(false);
  m_usage->setVisible(m_card.isImage());
  m_usage->setRange(0, qMax(1, m_card.capacity()));
  m_usage->setValue(m_card.usedBlocks());
  m_capacity->setText(
      m_card.isImage() ? QString("%1 save%5 · %2 of %3 blocks free · %4")
                             .arg(m_card.saves().size())
                             .arg(m_card.freeBlocks())
                             .arg(m_card.capacity())
                             .arg(m_card.japanese() ? "Japanese" : "Western")
                             .arg(m_card.saves().size() == 1 ? "" : "s")
      : m_card.path().isEmpty()
          ? "No card opened"
          : QString("Individual save · %1 blocks").arg(m_card.usedBlocks()));
  if (m_list->count())
    m_list->setCurrentRow(0);
  else
    selectSave();
  filter();
}
void MemoryCardManager::filter() {
  const auto q = m_search->text();
  for (int i = 0; i < m_list->count(); ++i) {
    auto *item = m_list->item(i);
    const auto &s = m_card.saves()[item->data(Qt::UserRole).toInt()];
    item->setHidden(!(item->text() + s.filename() + s.description())
                         .contains(q, Qt::CaseInsensitive));
  }
}
void MemoryCardManager::selectSave() {
  m_frames.clear();
  m_lastFrame = -1;
  m_icon->clear();
  m_banner->clear();
  auto *item = m_list->currentItem();
  if (!item) {
    m_title->setText(m_card.path().isEmpty() ? "Your saves, in one place"
                                             : "This card is empty");
    m_description->setText(
        "Import GCI, GCS or SAV files to keep your saves together.");
    m_info->clear();
    m_icon->setPixmap(shellIcon("memory-stick").pixmap(48, 48));
    return;
  }
  const auto &save = m_card.saves()[item->data(Qt::UserRole).toInt()];
  m_title->setText(m_titles.value(
      save.gameId(), save.title().isEmpty() ? save.filename() : save.title()));
  m_description->setText(save.description());
  const QString region = save.gameId()[3] == 'J'   ? "Japan"
                         : save.gameId()[3] == 'E' ? "USA"
                                                   : "Europe / other";
  m_info->setText(
      QString("%1\n%2 · %3\n%4 blocks · %5 KiB\nModified %6")
          .arg(save.filename(), save.gameId(), region)
          .arg(save.blocks())
          .arg(save.data.size() / 1024)
          .arg(save.modified().toLocalTime().toString("dd MMM yyyy · HH:mm")));
  const auto banner = save.banner();
  if (!banner.isNull())
    m_banner->setPixmap(QPixmap::fromImage(banner).scaled(
        m_banner->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
  m_frames = save.icons();
  if (save.pingPong() && m_frames.size() > 2)
    for (int i = m_frames.size() - 2; i > 0; --i)
      m_frames.append(m_frames[i]);
  m_animation.restart();
  animateIcon();
  if (m_frames.isEmpty())
    m_icon->setPixmap(shellIcon("memory-stick").pixmap(48, 48));
}
void MemoryCardManager::animateIcon() {
  if (!isVisible() || m_frames.isEmpty())
    return;
  int duration = 0;
  for (const auto &f : m_frames)
    duration += f.milliseconds;
  int t = int(m_animation.elapsed() % qMax(1, duration)), frame = 0;
  if (QSettings("GekkoAOT", "GekkoAOT")
          .value("ui/reducedMotion", false)
          .toBool())
    t = 0;
  while (frame < m_frames.size() - 1 && t >= m_frames[frame].milliseconds)
    t -= m_frames[frame++].milliseconds;
  if (frame != m_lastFrame) {
    m_lastFrame = frame;
    m_icon->setPixmap(
        QPixmap::fromImage(m_frames[frame].image)
            .scaled(64, 64, Qt::KeepAspectRatio, Qt::FastTransformation));
  }
}
QString MemoryCardManager::choosePath(const QString &title,
                                      const QString &filters,
                                      const QString &initial, bool save) {
  QFileDialog dialog(this, title, initial);
  dialog.setOption(QFileDialog::DontUseNativeDialog);
  dialog.setNameFilters(filters.split(";;"));
  if (save) {
    dialog.setDefaultSuffix(filters.contains("*.gci") ? "gci" : "raw");
    connect(&dialog, &QFileDialog::filterSelected, &dialog,
            [&dialog](const QString &filter) {
              QString extension = filter.contains("*.gcs")   ? "gcs"
                                  : filter.contains("*.sav") ? "sav"
                                  : filter.contains("*.gci") ? "gci"
                                                             : "raw";
              dialog.setDefaultSuffix(extension);
              auto selected = dialog.selectedFiles().value(0);
              const auto old = QFileInfo(selected).suffix().toLower();
              if (QStringList{"gci", "gcs", "sav"}.contains(old)) {
                selected.chop(old.size());
                dialog.selectFile(selected + extension);
              }
            });
  }
  dialog.setAcceptMode(save ? QFileDialog::AcceptSave
                            : QFileDialog::AcceptOpen);
  dialog.setFileMode(save ? QFileDialog::AnyFile : QFileDialog::ExistingFile);
  presentShellDialog(dialog, title, shellTVMode(this));
  if (execShellDialog(dialog) != QDialog::Accepted)
    return {};
  return dialog.selectedFiles().value(0);
}
bool MemoryCardManager::confirm(const QString &title, const QString &text,
                                const QString &accept) {
  QDialog dialog(this);
  auto *layout = new QVBoxLayout(&dialog);
  auto *label = new QLabel(text);
  label->setWordWrap(true);
  layout->addWidget(label);
  auto *box = new QDialogButtonBox(QDialogButtonBox::Cancel);
  box->addButton(accept, QDialogButtonBox::AcceptRole);
  layout->addWidget(box);
  connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  presentShellDialog(dialog, title, shellTVMode(this));
  return execShellDialog(dialog) == QDialog::Accepted;
}
void MemoryCardManager::openFile() {
  const auto path =
      choosePath("Open memory card", CardFilters, m_card.path(), false);
  if (!path.isEmpty())
    openPath(path);
}
void MemoryCardManager::createCard() {
  QDialog options(this);
  auto *layout = new QVBoxLayout(&options);
  auto *form = new QFormLayout;
  auto *size = new QComboBox;
  for (int n : {59, 123, 251, 507, 1019, 2043})
    size->addItem(QString("%1 blocks").arg(n), n);
  size->setCurrentIndex(2);
  form->addRow("Capacity", size);
  auto *encoding = new QComboBox;
  encoding->addItems({"Western", "Japanese"});
  form->addRow("Encoding", encoding);
  layout->addLayout(form);
  auto *note =
      new QLabel("251 blocks matches GekkoAOT’s current game save cards.");
  note->setWordWrap(true);
  layout->addWidget(note);
  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &options, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &options, &QDialog::reject);
  presentShellDialog(options, "New memory card", shellTVMode(this));
  if (execShellDialog(options) != QDialog::Accepted)
    return;
  const auto path = choosePath(
      "Create memory card", "Memory card (*.raw *.gcp)",
      QDir(m_currentUserDir)
          .filePath(
              QFileInfo::exists(
                  QDir(m_currentUserDir).filePath("cards/MemoryCardA.raw"))
                  ? "cards/NewCard.raw"
                  : "cards/MemoryCardA.raw"),
      true);
  if (path.isEmpty())
    return;
  MemoryCards::Card card;
  QString error;
  if (!card.create(path, size->currentData().toInt(),
                   encoding->currentIndex() == 1, &error)) {
    message(error, true);
    return;
  }
  openPath(path);
  message("New memory card created.");
}
void MemoryCardManager::importFiles() {
  if (locked() || !m_card.isImage())
    return;
  QFileDialog browser(this, "Import saves");
  browser.setOption(QFileDialog::DontUseNativeDialog);
  browser.setNameFilter("GameCube saves (*.gci *.gcs *.sav)");
  browser.setFileMode(QFileDialog::ExistingFiles);
  presentShellDialog(browser, "Import saves", shellTVMode(this));
  if (execShellDialog(browser) != QDialog::Accepted)
    return;
  QList<MemoryCards::Save> saves;
  QStringList duplicates;
  QString error;
  for (const auto &file : browser.selectedFiles()) {
    MemoryCards::Save save;
    if (!MemoryCards::Card::readSave(file, save, &error)) {
      message(QFileInfo(file).fileName() + ": " + error, true);
      return;
    }
    for (const auto &existing : m_card.saves())
      if (existing.identity() == save.identity())
        duplicates << save.filename();
    saves << save;
  }
  if (!duplicates.isEmpty() &&
      !confirm("Replace saves?",
               QString("Replace these existing saves?\n\n%1\n\nA backup of the "
                       "card will be kept.")
                   .arg(duplicates.join('\n')),
               "Replace"))
    return;
  auto staged = m_card;
  if (!staged.importSaves(saves, !duplicates.isEmpty(), &error) ||
      !staged.commit(&error)) {
    message(error, true);
    return;
  }
  m_card = std::move(staged);
  refresh();
  message(
      QString("Imported %1 saves. Previous card backed up.").arg(saves.size()));
}
void MemoryCardManager::exportSelected(bool all) {
  QList<int> indices;
  if (all) {
    for (int i = 0; i < m_card.saves().size(); ++i)
      indices << i;
  } else
    for (auto *item : m_list->selectedItems())
      indices << item->data(Qt::UserRole).toInt();
  if (indices.isEmpty())
    return;
  QString error;
  if (indices.size() == 1 && !all) {
    const auto &save = m_card.saves()[indices.first()];
    QString path = choosePath(
        "Export save",
        "GCI save (*.gci);;GameShark save (*.gcs);;MaxDrive save (*.sav)",
        QDir(QFileInfo(m_card.path()).absolutePath())
            .filePath(save.exportFilename() + ".gci"),
        true);
    if (path.isEmpty())
      return;
    if (QFileInfo(path).absoluteFilePath() == m_card.path() ||
        (!QFileInfo(path).canonicalFilePath().isEmpty() &&
         QFileInfo(path).canonicalFilePath() ==
             QFileInfo(m_card.path()).canonicalFilePath())) {
      message("Choose a destination different from the opened file.", true);
      return;
    }
    if (QFileInfo(path).suffix().isEmpty())
      path += ".gci";
    const auto ext = QFileInfo(path).suffix().toLower();
    if (ext != "gci" && ext != "gcs" && ext != "sav") {
      message("Choose GCI, GCS or SAV as the export format.", true);
      return;
    }
    if (!MemoryCards::Card::writeSave(
            path, save,
            ext == "gcs"   ? MemoryCards::SaveFormat::GCS
            : ext == "sav" ? MemoryCards::SaveFormat::SAV
                           : MemoryCards::SaveFormat::GCI,
            &error)) {
      message(error, true);
      return;
    }
  } else {
    const auto directory =
        shellChooseFiles(this, "Export saves",
                         QFileInfo(m_card.path()).absolutePath(), false, true)
            .value(0);
    if (directory.isEmpty())
      return;
    for (int index : indices) {
      const auto &save = m_card.saves()[index];
      QString path = QDir(directory).filePath(save.exportFilename() + ".gci");
      int suffix = 2;
      while (QFileInfo::exists(path))
        path = QDir(directory).filePath(save.exportFilename() +
                                        QString(" (%1).gci").arg(suffix++));
      if (!MemoryCards::Card::writeSave(path, save,
                                        MemoryCards::SaveFormat::GCI, &error)) {
        message(error, true);
        return;
      }
    }
  }
  message(QString("Exported %1 saves.").arg(indices.size()));
}
void MemoryCardManager::backupCard() {
  const auto path = choosePath("Back up card", "Memory cards (*.raw *.gcp)",
                               m_card.path() + ".copy.raw", true);
  if (path.isEmpty())
    return;
  QString error;
  if (!m_card.exportImage(path, &error))
    message(error, true);
  else
    message("Card backup saved.");
}
void MemoryCardManager::removeSelected() {
  if (locked())
    return;
  QList<int> indices;
  for (auto *item : m_list->selectedItems())
    indices << item->data(Qt::UserRole).toInt();
  if (indices.isEmpty())
    return;
  if (!confirm("Delete saves?",
               QString("Delete %1 selected saves from this card? A backup of "
                       "the card will be kept.")
                   .arg(indices.size()),
               "Delete"))
    return;
  QString error;
  auto staged = m_card;
  if (!staged.removeSaves(indices, &error) || !staged.commit(&error)) {
    message(error, true);
    return;
  }
  m_card = std::move(staged);
  refresh();
  message("Saves deleted. Previous card backed up.");
}
