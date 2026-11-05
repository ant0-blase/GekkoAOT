#include "GameLibrary.hpp"
#include "DiscMetadata.hpp"
#include "SettingsStyle.hpp"
#include "ShellStyle.hpp"
#include "ShellNavigation.hpp"
#include "GameCard.hpp"
#include "ShellMenu.hpp"
#include <QAction>
#include <QDesktopServices>
#include <QUrl>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <memory>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QGridLayout>
#include <QSettings>
#include <QApplication>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QShortcut>
#include <algorithm>

namespace
{
QString normalized(const QString &path)
{
    QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}
QByteArray bytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QString cleanGameText(QString text)
{
    // Some old metadata caches decoded Western banner punctuation as C1
    // control characters. Keep existing game names without missing-glyph boxes.
    const QMap<ushort,ushort> punctuation={{0x80,0x20ac},{0x85,0x2026},{0x91,0x2018},{0x92,0x2019},{0x93,0x201c},{0x94,0x201d},{0x96,0x2013},{0x97,0x2014},{0x99,0x2122}};
    for(auto& character:text)if(punctuation.contains(character.unicode()))character=QChar(punctuation.value(character.unicode()));
    return text;
}
QString duration(qint64 seconds) {
    if(!seconds)return "—";
    if(seconds<60)return "< 1 min";
    if(seconds<3600)return QString("%1 min").arg(seconds/60);
    return QString("%1 h %2 min").arg(seconds/3600).arg((seconds/60)%60);
}
class LibraryItem final : public QTreeWidgetItem
{
  public:
    bool operator<(const QTreeWidgetItem &other) const override
    {
        const int col = treeWidget()->sortColumn();
        if (col == 5 || col == 6 || col == 7)
            return data(col, Qt::UserRole).toLongLong() < other.data(col, Qt::UserRole).toLongLong();
        return text(col).localeAwareCompare(other.text(col)) < 0;
    }
};
} // namespace

GameLibrary::GameLibrary(const QString &state, const QString &reader, QWidget *parent)
    : QWidget(parent), m_root(QDir(state).filePath("library")), m_reader(reader)
{
    setObjectName("gameLibrary");
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(R"(
        QWidget#gameLibrary { background: transparent; color: #eeeeee; border-radius: 22px; }
        QTreeWidget { background: transparent; color: #eeeeee; border: none; outline: 0; padding: 0; }
        QTreeWidget::item { padding: 5px 10px; border: none; border-bottom: 1px solid rgba(255,255,255,7); border-radius: 0; }
        QTreeWidget::item:selected { background: rgba(255,255,255,17); color: white; }
        QTreeWidget::item:hover { background: rgba(255,255,255,8); }
        QHeaderView { background: transparent; border: none; padding: 0; }
        QHeaderView::section { background: #252525; color: #a5a5a5; padding: 8px 10px; border: none; border-bottom: 1px solid rgba(255,255,255,15); border-radius: 0; font-size: 12px; }
        QLabel { color: #aaaaaa; background: transparent; }
        QPushButton,QLineEdit { min-height: 30px; padding: 8px 14px; border: 1px solid rgba(255,255,255,28); border-radius: 14px; color: #eeeeee; background: rgba(255,255,255,8); }
        QPushButton:hover { background: rgba(255,255,255,20); }
        QPushButton:focus,QLineEdit:focus { border-color: #eeeeee; }
    )");
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 18, 18, 12);
    outer->setSpacing(12);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel("LIBRARY");
    title->setFont(shellDisplayFont(26));
    title->setStyleSheet("color: #eeeeee; padding: 3px 0;");
    heading->addWidget(title);
    auto* import=new QPushButton(shellIcon("folder-plus"),"Import folder");import->setObjectName("libraryImportFolder");import->setToolTip("Add games from a folder and its subfolders");import->setStyleSheet("QPushButton {background:transparent;border:1px solid transparent;padding:6px 12px;border-radius:12px;color:#bcbcbc;} QPushButton:hover {background:rgba(255,255,255,10);color:#eeeeee;} QPushButton:focus {border-color:#eeeeee;}");heading->addWidget(import);connect(import,&QPushButton::clicked,this,&GameLibrary::importFolder);
    heading->addStretch();
    heading->addWidget(shellDotDisc(this));
    outer->addLayout(heading);
    auto* controls = new QWidget(this);controls->setObjectName("libraryControls");
    auto *header = new QHBoxLayout(controls);header->setContentsMargins(0,0,0,0);
    m_search = new QLineEdit;
    m_search->setObjectName("librarySearch");
    m_search->setPlaceholderText("Search games, Game ID or publisher…");
    m_search->setClearButtonEnabled(true);
    header->addWidget(m_search, 1);
    m_search->addAction(shellIcon("search"), QLineEdit::LeadingPosition);
    outer->addWidget(controls);
    m_list = new QTreeWidget;
    m_list->setObjectName("libraryGames");
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(false);
    m_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_list->setAllColumnsShowFocus(true);
    m_list->setUniformRowHeights(true);
    m_list->header()->setFixedHeight(40);
    m_list->setHeaderLabels(
        {"", "Game", "Game ID", "Publisher", "Region", "Size", "Last played", "Play time"});
    m_list->setIconSize(QSize(112, 38));
    m_list->setColumnWidth(0, 132);
    m_list->header()->setSectionResizeMode(1,QHeaderView::Stretch);
    m_list->setColumnWidth(2, 92);
    m_list->setColumnWidth(3, 175);
    m_list->setColumnWidth(4, 85);
    m_list->setColumnWidth(5, 112);
    m_list->setColumnWidth(6, 150);
    m_list->setColumnWidth(7, 110);
    m_list->setColumnHidden(3,true);m_list->setColumnHidden(4,true);
    m_list->header()->setStretchLastSection(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setSortingEnabled(true);
    m_list->sortByColumn(1, Qt::AscendingOrder);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_views = new QStackedWidget;
    m_views->addWidget(m_list);
    m_gridScroll = new QScrollArea;
    m_gridScroll->setObjectName("libraryCardView");
    m_gridScroll->setFrameShape(QFrame::NoFrame);
    m_gridScroll->setWidgetResizable(true);
    m_gridScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_gridScroll->setStyleSheet("QScrollArea { background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }");
    m_gridContent = new QWidget;
    m_gridLayout = new QGridLayout(m_gridContent);
    m_gridLayout->setContentsMargins(0,0,0,0);
    m_gridLayout->setSpacing(2);
    m_gridLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_gridScroll->setWidget(m_gridContent);
    m_gridScroll->viewport()->installEventFilter(this);
    m_views->addWidget(m_gridScroll);
    outer->addWidget(m_views, 1);
    m_gridMode = QSettings("GekkoAOT","GekkoAOT").value("library/cards",true).toBool();
    m_views->setCurrentWidget(m_gridMode ? static_cast<QWidget*>(m_gridScroll) : static_cast<QWidget*>(m_list));
    auto* viewShortcut=new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_L),this);viewShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(viewShortcut,&QShortcut::activated,this,&GameLibrary::toggleView);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this,&QWidget::customContextMenuRequested,this,[this](const QPoint& point){contextMenu({},mapToGlobal(point));});
    m_gridContent->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_gridContent,&QWidget::customContextMenuRequested,this,[this](const QPoint& point){contextMenu({},m_gridContent->mapToGlobal(point));});
    m_gridRefresh=new QTimer(this);m_gridRefresh->setSingleShot(true);m_gridRefresh->setInterval(60);
    connect(m_gridRefresh,&QTimer::timeout,this,&GameLibrary::rebuildGrid);
    auto* selectAll=new QShortcut(QKeySequence::SelectAll,this);
    selectAll->setContext(Qt::WidgetWithChildrenShortcut);
    connect(selectAll,&QShortcut::activated,this,[this]{if(!m_search->hasFocus())m_list->selectAll();else m_search->selectAll();});
    auto *footer = new QHBoxLayout;
    auto* version=new QLabel(QString("GekkoAOT v%1").arg(QStringLiteral(GEKKOAOT_VERSION)));version->setObjectName("launcherVersion");version->setStyleSheet("color:#858585;");footer->addWidget(version);footer->addSpacing(12);
    m_count = new QLabel;
    footer->addWidget(m_count);
    footer->addStretch();
    footer->addWidget(new QLabel("Joystick  Navigate    A / ×  Select    B / ○  Back"));
    outer->addLayout(footer);
    connect(m_search, &QLineEdit::textChanged, this, [&] { filter(); });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this,
            [&]
            {
                if (selected && !selectedPath().isEmpty())
                    selected(selectedPath());
                syncCardSelection();
            });
    connect(m_list, &QTreeWidget::itemActivated, this,
            [&](QTreeWidgetItem *row, int)
            {
                if (activated)
                    activated(row->data(0, Qt::UserRole).toString());
            });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this,
            [&](QPoint point)
            {
                auto *row = m_list->itemAt(point);
                if (!row) {contextMenu({},m_list->viewport()->mapToGlobal(point));return;}
                if (!row->isSelected()) m_list->setCurrentItem(row);
                contextMenu(row->data(0, Qt::UserRole).toString(),m_list->viewport()->mapToGlobal(point));
            });
    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(30000);
    connect(m_timeout, &QTimer::timeout, this, [&] { m_process.kill(); });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) { finishRead(code, status); });
    connect(&m_process, &QProcess::errorOccurred, this,
            [&](QProcess::ProcessError e)
            {
                if (e != QProcess::FailedToStart)
                    return;
                m_timeout->stop();
                if (error)
                    error("Disc metadata reader could not start: " + m_process.errorString());
                m_activePath.clear();
                readNext();
            });
    load();
}

void GameLibrary::finishRead(int code, QProcess::ExitStatus status)
{
    m_timeout->stop();
    const QString path = m_activePath;
    if (m_games.contains(path))
    {
        auto entry = m_games[path];
        QFileInfo info(path);
        if (code == 0 && status == QProcess::NormalExit)
        {
            const auto meta = readDiscMetadata(m_staging);
            if (!meta.isEmpty())
            {
                const QString target = cachePath(path);
                QDir(target).removeRecursively();
                if (QDir().rename(m_staging, target))
                {
                    for (auto i = meta.begin(); i != meta.end(); ++i)
                        entry[i.key()] = i.value();
                    entry["metadataSize"] = QString::number(m_sourceSize);
                    entry["metadataMtime"] = QString::number(m_sourceMtime);
                    entry.remove("metadataError");
                }
                else
                {
                    entry["metadataError"] = "Could not commit disc metadata cache";
                    if (error)
                        error(entry["metadataError"].toString());
                }
            }
        }
        else
        {
            const QString detail = QString::fromUtf8(m_process.readAllStandardError()).trimmed();
            entry["metadataError"] = detail.isEmpty() ? "Could not read disc metadata" : detail;
            if (error)
                error(info.fileName() + ": " + entry["metadataError"].toString());
        }
        m_games[path] = entry;
        refreshRow(path);
        save();
        if (metadataReady)
            metadataReady(path);
    }
    QDir(m_staging).removeRecursively();
    m_activePath.clear();
    m_staging.clear();
    if (m_games.contains(path))
    {
        const QFileInfo latest(path);
        if (latest.size() != m_sourceSize || latest.lastModified().toMSecsSinceEpoch() != m_sourceMtime)
            addGame(path);
    }
    readNext();
}

QString GameLibrary::cachePath(const QString &path) const
{
    return discMetadataCachePath(QDir(m_root).absoluteFilePath(".."), path);
}
void GameLibrary::load()
{
    const auto root = QJsonDocument::fromJson(bytes(QDir(m_root).filePath("games.json"))).object();
    m_legacyImported = root["legacyImported"].toBool();
    for (const auto &item : root["games"].toArray())
    {
        auto game = item.toObject();
        for(const auto& field:{"title","publisher","comment"})game[field]=cleanGameText(game[field].toString());
        const auto path = game["path"].toString();
        if (!path.isEmpty())
        {
            m_games[path] = game;
            refreshRow(path);
        }
    }
    for (const auto &path : m_games.keys())
        addGame(path);
    filter();
}
void GameLibrary::save()
{
    QDir().mkpath(m_root);
    QJsonArray games;
    for (const auto &game : m_games)
        games.append(game);
    QSaveFile file(QDir(m_root).filePath("games.json"));
    const QByteArray data =
        QJsonDocument(QJsonObject{{"version", 1}, {"legacyImported", m_legacyImported}, {"games", games}})
            .toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        if (error)
            error("Could not save game history: " + file.errorString());
    if (changed) changed();
    if(m_gridRefresh)m_gridRefresh->start();
}
void GameLibrary::importCachedGames()
{
    if (m_legacyImported)
        return;
    m_legacyImported = true;
    const QDir discs(QDir(m_root).filePath("../cache/discs"));
    for (const auto &slug : discs.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        addGame(QJsonDocument::fromJson(bytes(discs.filePath(slug + "/meta/disc-source.json")))
                    .object()["source"]
                    .toString());
    save();
}
void GameLibrary::addGame(const QString &input)
{
    if (input.isEmpty())
        return;
    const auto path = normalized(input);
    const QFileInfo info(path);
    if (!info.isFile())
        return;
    auto entry = m_games.value(path);
    entry["path"] = path;
    entry["size"] = QString::number(info.size());
    if (entry["title"].toString().isEmpty())
        entry["title"] = info.completeBaseName();
    const bool stale =
        entry["metadataSize"].toString() != QString::number(info.size()) ||
        entry["metadataMtime"].toString() != QString::number(info.lastModified().toMSecsSinceEpoch());
    if(m_games.contains(path) && !stale && entry==m_games.value(path))return;
    m_games[path] = entry;
    refreshRow(path);
    save();
    if (stale && !m_pending.contains(path) && m_activePath != path)
    {
        m_pending.append(path);
        QTimer::singleShot(0, this, [&] { readNext(); });
    }
}
void GameLibrary::readNext()
{
    if (!m_activePath.isEmpty() || m_pending.isEmpty())
        return;
    if (!QFileInfo::exists(m_reader))
    {
        m_pending.clear();
        if (error)
            error("Disc metadata reader is missing; install gekkoaot-native-disc beside the launcher.");
        return;
    }
    m_activePath = m_pending.takeFirst();
    const QFileInfo sourceInfo(m_activePath);
    m_sourceSize = sourceInfo.size();
    m_sourceMtime = sourceInfo.lastModified().toMSecsSinceEpoch();
    m_staging = cachePath(m_activePath) + ".reading-" + QUuid::createUuid().toString(QUuid::Id128);
    QDir().mkpath(QFileInfo(m_staging).absolutePath());
    if (QFileInfo(m_activePath).suffix().compare("dol", Qt::CaseInsensitive) == 0)
    {
        // An extracted DOL may have system files beside it; no image decoding.
        const QDir dir = QFileInfo(m_activePath).dir();
        const QDir root(dir.dirName().compare("sys", Qt::CaseInsensitive) == 0 ? dir.absoluteFilePath("..")
                                                                               : dir.absolutePath());
        QDir().mkpath(m_staging + "/sys");
        QDir().mkpath(m_staging + "/meta");
        for (const auto &name : {"boot.bin", "bi2.bin"})
            QFile::copy(root.filePath(QString("sys/") + name), m_staging + "/sys/" + name);
        for (const auto &name : {"files/opening.bnr", "opening.bnr", "assets/opening.bnr"})
            if (QFile::copy(root.filePath(name), m_staging + "/meta/opening.bnr"))
                break;
        // Completion shares the normal validation/cache path.
        QTimer::singleShot(0, this, [&] { finishRead(0, QProcess::NormalExit); });
        return;
    }
    m_process.setProgram(m_reader);
    m_process.setArguments({"--metadata", m_activePath, m_staging});
    m_process.start();
    m_timeout->start();
}
QJsonObject GameLibrary::metadata(const QString &path) const { return m_games.value(normalized(path)); }
QList<LibraryEntry> GameLibrary::entries() const
{
    QList<LibraryEntry> result;
    for (auto it = m_games.cbegin(); it != m_games.cend(); ++it) {
        const auto& value = it.value();
        result.append({it.key(), value["title"].toString(), value["gameId"].toString(),
            value["publisher"].toString(), value["region"].toString(), value["comment"].toString(),
            decodeGameCubeBanner(bytes(cachePath(it.key()) + "/meta/opening.bnr")),
            value["lastPlayed"].toString().toLongLong(), value["playSeconds"].toString().toLongLong()});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.title.localeAwareCompare(b.title) < 0;
    });
    return result;
}
QString GameLibrary::selectedPath() const
{
    auto *row = m_list->currentItem();
    if(row && !row->isSelected())row=m_list->selectedItems().value(0,nullptr);
    return row ? row->data(0, Qt::UserRole).toString() : QString{};
}
QStringList GameLibrary::selectedPaths() const
{
    QStringList paths;
    for (auto* row : m_list->selectedItems()) paths << row->data(0, Qt::UserRole).toString();
    return paths;
}
void GameLibrary::selectGame(const QString &path)
{
    const auto key = normalized(path);
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
    {
        auto *row = m_list->topLevelItem(i);
        if (row->data(0, Qt::UserRole).toString() == key)
        {
            const QSignalBlocker block(m_list);
            if (!row->isSelected()) m_list->setCurrentItem(row);
            syncCardSelection();
            break;
        }
    }
}
void GameLibrary::recordLaunch(const QString &input)
{
    const auto path = normalized(input);
    addGame(path);
    m_games[path]["lastPlayed"] = QString::number(QDateTime::currentMSecsSinceEpoch());
    refreshRow(path);
    save();
}
void GameLibrary::addPlayTime(const QString &input, qint64 seconds)
{
    const auto path = normalized(input);
    if (!m_games.contains(path) || seconds <= 0)
        return;
    m_games[path]["playSeconds"] =
        QString::number(m_games[path]["playSeconds"].toString().toLongLong() + seconds);
    refreshRow(path);
    save();
}
void GameLibrary::refreshRow(const QString &path)
{
    auto *row = static_cast<QTreeWidgetItem *>(nullptr);
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        if (m_list->topLevelItem(i)->data(0, Qt::UserRole).toString() == path)
        {
            row = m_list->topLevelItem(i);
            break;
        }
    if (!row)
    {
        row = new LibraryItem;
        m_list->addTopLevelItem(row);
        row->setData(0, Qt::UserRole, path);
    }
    const auto e = m_games[path];
    row->setSizeHint(0,QSize(132,60));
    row->setText(1, e["title"].toString());
    row->setText(2, e["gameId"].toString());
    row->setText(3, e["publisher"].toString());
    row->setText(4, e["region"].toString());
    const auto size = e["size"].toString().toLongLong(), played = e["lastPlayed"].toString().toLongLong(),
               seconds = e["playSeconds"].toString().toLongLong();
    row->setText(5, QLocale().formattedDataSize(size));
    row->setData(5, Qt::UserRole, size);
    const auto date=QDateTime::fromMSecsSinceEpoch(played);
    row->setText(6, !played?QString("—"):date.date()==QDate::currentDate()?QString("Today · ")+date.toString("HH:mm"):date.toString("dd MMM yyyy"));
    row->setData(6, Qt::UserRole, played);
    row->setText(7, duration(seconds));
    row->setData(7, Qt::UserRole, seconds);
    const auto banner = decodeGameCubeBanner(bytes(cachePath(path) + "/meta/opening.bnr"));
    if (!banner.isNull())
        row->setIcon(0, QIcon(QPixmap::fromImage(banner).scaled(112, 38, Qt::KeepAspectRatio,
                                                                Qt::SmoothTransformation)));
    else
    {
        QPixmap placeholder(144, 48);
        placeholder.fill(QColor("#3c3c3c"));
        row->setIcon(0, QIcon(placeholder));
    }
    for (int col = 0; col < 8; ++col)
        row->setToolTip(col, e["comment"].toString() + "\n" + path +
                                 (e.contains("metadataError") ? "\n" + e["metadataError"].toString() : ""));
    if (!QFileInfo::exists(path))
        for (int col = 0; col < 8; ++col)
            row->setForeground(col, QColor("#888888"));
    filter();
}
void GameLibrary::filter()
{
    const QString query = m_search->text().trimmed();
    int visible = 0;
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
    {
        auto *row = m_list->topLevelItem(i);
        bool match = query.isEmpty();
        for (int c = 1; c < 5 && !match; ++c)
            match = row->text(c).contains(query, Qt::CaseInsensitive);
        row->setHidden(!match);
        visible += match;
    }
    m_count->setText(QString("%1 game%2%3")
                         .arg(m_games.size())
                         .arg(m_games.size() == 1 ? "" : "s")
                         .arg(query.isEmpty() ? QString{} : QString(" · %1 shown").arg(visible)));
    if(m_gridRefresh)m_gridRefresh->start();
}
void GameLibrary::properties(const QString &path)
{
    const auto e = m_games[path];
    QDialog dialog(this);
    dialog.setWindowTitle("Game Properties");
    dialog.setStyleSheet(settingsStyleSheet());
    dialog.resize(620, 500);
    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(24, 20, 24, 20);
    outer->setSpacing(16);
    auto *banner = new QLabel;
    const auto image = decodeGameCubeBanner(bytes(cachePath(path) + "/meta/opening.bnr"));
    if (!image.isNull())
        banner->setPixmap(QPixmap::fromImage(image).scaled(384, 128));
    banner->setAlignment(Qt::AlignCenter);
    outer->addWidget(banner);
    auto *form = new QFormLayout;
    form->setVerticalSpacing(12);
    for (const auto &field : QList<QPair<QString, QString>>{{"Title", "title"},
                                                            {"Game ID", "gameId"},
                                                            {"Game code", "gameCode"},
                                                            {"Maker code", "makerCode"},
                                                            {"Publisher", "publisher"},
                                                            {"Region", "region"},
                                                            {"Comment", "comment"},
                                                            {"Source", "path"}})
    {
        auto *value = new QLabel(e[field.second].toString());
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(field.first, value);
    }
    form->addRow("Disc / revision",
                 new QLabel(QString("%1 / %2").arg(e["discNumber"].toInt(1)).arg(e["revision"].toInt())));
    outer->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(buttons);
    presentShellDialog(dialog,"Game properties",shellTVMode(this));
    execShellDialog(dialog);
}

void GameLibrary::rebuildGrid()
{
    if(!m_gridMode)return;
    QList<LibraryEntry> visible;
    const QString queryText=m_search->text().trimmed();
    for(const auto& entry:entries())if(queryText.isEmpty() || (entry.title+entry.gameId+entry.publisher).contains(queryText,Qt::CaseInsensitive))visible<<entry;
    bool same=visible.size()==m_cards.size();
    for(int i=0;same && i<visible.size();++i)same=m_cards[i]->game.path==visible[i].path && m_cards[i]->game.title==visible[i].title && m_cards[i]->game.banner==visible[i].banner;
    if(same){syncCardSelection();return;}
    const int scroll=m_gridScroll->verticalScrollBar()->value();
    QString focused;
    for(auto* card:m_cards)if(card->hasFocus())focused=card->game.path;
    while(auto* item=m_gridLayout->takeAt(0))delete item;
    for(auto* card:m_cards){card->hide();card->deleteLater();}m_cards.clear();
    const QString query=m_search->text().trimmed();
    const bool reduced=QSettings("GekkoAOT","GekkoAOT").value("ui/reducedMotion",false).toBool();
    for(const auto& entry:entries()) {
        if(!query.isEmpty() && !(entry.title+entry.gameId+entry.publisher).contains(query,Qt::CaseInsensitive))continue;
        auto* card=new GameCard(entry,m_gridContent);card->setObjectName("desktopGameCard");
        card->setCheckable(true);card->setReducedMotion(reduced);card->installEventFilter(this);
        card->setActions([this,card]{contextMenu(card->game.path,card->mapToGlobal(QPoint(card->width()-40,card->height()-60)));});
        card->doubleClicked=[this,path=entry.path]{if(activated)activated(path);};
        connect(card,&QAbstractButton::clicked,this,[this,card] {
            auto* row=static_cast<QTreeWidgetItem*>(nullptr);
            for(int i=0;i<m_list->topLevelItemCount();++i)if(m_list->topLevelItem(i)->data(0,Qt::UserRole).toString()==card->game.path)row=m_list->topLevelItem(i);
            if(!row)return;
            const auto modifiers=QApplication::keyboardModifiers();
            if(modifiers.testFlag(Qt::ShiftModifier) && !selectedPath().isEmpty()) {
                int anchor=0,target=m_cards.indexOf(card);
                for(int i=0;i<m_cards.size();++i)if(m_cards[i]->game.path==selectedPath())anchor=i;
                {
                    QSignalBlocker block(m_list);
                    if(!modifiers.testFlag(Qt::ControlModifier))m_list->clearSelection();
                    for(int i=qMin(anchor,target);i<=qMax(anchor,target);++i)
                        for(int r=0;r<m_list->topLevelItemCount();++r)
                            if(m_list->topLevelItem(r)->data(0,Qt::UserRole).toString()==m_cards[i]->game.path)m_list->topLevelItem(r)->setSelected(true);
                    m_list->setCurrentItem(row,0,QItemSelectionModel::NoUpdate);
                }
                if(selected)selected(card->game.path);
            } else m_list->setCurrentItem(row,0,modifiers.testFlag(Qt::ControlModifier)?QItemSelectionModel::Toggle:QItemSelectionModel::ClearAndSelect);
            syncCardSelection();
        });
        card->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(card,&QWidget::customContextMenuRequested,this,[this,card](const QPoint& point){
            if(!card->isChecked())card->click();
            contextMenu(card->game.path,card->mapToGlobal(point));
        });
        m_cards<<card;card->show();
    }
    reflowGrid();m_gridLayout->activate();syncCardSelection();
    QTimer::singleShot(0,this,[this,focused,scroll]{
        if(!focused.isEmpty())for(auto* c:m_cards)if(c->game.path==focused)c->setFocus();
        m_gridScroll->verticalScrollBar()->setValue(scroll);
    });
}
void GameLibrary::reflowGrid()
{
    if(!m_gridMode || !m_gridScroll)return;
    const int width=qMax(220,m_gridScroll->viewport()->width()-4);
    const int columns=qMax(1,width/242);
    m_gridColumns=columns;
    const int cardWidth=qBound(210,(width-(columns-1)*2)/columns,360);
    for(int i=0;i<m_cards.size();++i){m_cards[i]->setFixedSize(cardWidth,190);m_gridLayout->addWidget(m_cards[i],i/columns,i%columns);}
}
void GameLibrary::syncCardSelection()
{
    const auto paths=selectedPaths();for(auto* card:m_cards){card->setChecked(paths.contains(card->game.path));card->update();}
}
void GameLibrary::resizeEvent(QResizeEvent* event){QWidget::resizeEvent(event);reflowGrid();}
bool GameLibrary::eventFilter(QObject* watched,QEvent* event)
{
    if(watched==m_gridScroll->viewport()&&event->type()==QEvent::Resize)reflowGrid();
    if(event->type()==QEvent::KeyPress)if(auto* card=dynamic_cast<GameCard*>(watched)) {
        const int key=static_cast<QKeyEvent*>(event)->key();
        if(key==Qt::Key_Return || key==Qt::Key_Enter){if(activated)activated(card->game.path);return true;}
        if(navigateGames(key))return true;
    }
    return QWidget::eventFilter(watched,event);
}
bool GameLibrary::navigateGames(int key)
{
    auto* focus=QApplication::focusWidget();
    if(!m_gridMode && focus && (focus==m_list || m_list->isAncestorOf(focus))) {
        if(key==Qt::Key_Up && m_list->indexOfTopLevelItem(m_list->currentItem())==0){shellFocusControl(m_search);return true;}
        return false;
    }
    GameCard* card=nullptr;
    for(auto* p=focus;p && p!=this;p=p->parentWidget())if((card=dynamic_cast<GameCard*>(p)))break;
    if(!card)return false;
    const int index=m_cards.indexOf(card);
    if(index<0)return false;
    if(key==Qt::Key_Return || key==Qt::Key_Enter){if(activated)activated(card->game.path);return true;}
    if(key==Qt::Key_Up && index<m_gridColumns){shellFocusControl(m_search);return true;}
    int next=index;
    if(key==Qt::Key_Up)next=qMax(0,index-m_gridColumns);
    else if(key==Qt::Key_Down)next=qMin(int(m_cards.size())-1,index+m_gridColumns);
    else if(key==Qt::Key_Left){if(index%m_gridColumns)next=index-1;}
    else if(key==Qt::Key_Right){if(index%m_gridColumns<m_gridColumns-1 && index+1<m_cards.size())next=index+1;}
    else return false;
    auto* target=m_cards[next];shellFocusControl(target);
    if(next!=index && !target->isChecked())target->click();
    return true;
}
void GameLibrary::contextMenu(const QString& path,const QPoint& global)
{
    ShellMenu menu(window(),global);
    QAction *play=nullptr,*compile=nullptr,*details=nullptr,*folder=nullptr,*remove=nullptr;
    if(!path.isEmpty()) {
        play=menu.addAction(shellIcon("play"),"Play");
        compile=menu.addAction(shellIcon("cpu"),"Compile selected");
        details=menu.addAction(shellIcon("file-text"),"Properties…");menu.addSeparator();
        folder=menu.addAction(shellIcon("folder-open"),"Open game folder");
        remove=menu.addAction(shellIcon("trash"),"Remove from history");menu.addSeparator();
    }
    auto* add=menu.addAction(shellIcon("folder-plus"),"Add games…");add->setObjectName("libraryAddGames");
    auto* view=menu.addAction(shellIcon(m_gridMode?"list-video":"library"),m_gridMode?"Show list":"Show cards");view->setObjectName("libraryViewToggle");view->setShortcut(QKeySequence(Qt::CTRL|Qt::Key_L));
    menu.ensurePolished();menu.adjustSize();
    const QRect area(window()->mapToGlobal(QPoint{}),window()->size());
    const QPoint at(qBound(area.left()+20,global.x(),qMax(area.left()+20,area.right()-menu.width()-20)),qBound(area.top()+20,global.y(),qMax(area.top()+20,area.bottom()-menu.height()-20)));
    auto* chosen=menu.exec(at);
    if(!chosen)return;
    if(chosen==add) {for(const auto& file:shellChooseFiles(this,"Add games",{},true))addGame(file);}
    else if(chosen==view)toggleView();
    else if(chosen==play&&activated)activated(path);
    else if(chosen==compile&&compileRequested)compileRequested(selectedPaths());
    else if(chosen==details)properties(path);
    else if(chosen==folder)QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    else if(chosen==remove){
        m_games.remove(path);m_pending.removeAll(path);
        for(int i=0;i<m_list->topLevelItemCount();++i)if(m_list->topLevelItem(i)->data(0,Qt::UserRole).toString()==path){delete m_list->topLevelItem(i);break;}
        save();filter();
    }
}

void GameLibrary::toggleView()
{
    m_gridMode=!m_gridMode;
    m_views->setCurrentWidget(m_gridMode ? static_cast<QWidget*>(m_gridScroll) : static_cast<QWidget*>(m_list));
    QSettings("GekkoAOT","GekkoAOT").setValue("library/cards",m_gridMode);
    if(m_gridMode)rebuildGrid();
    focusGames();shellRevealSnapshot(m_views);
}

void GameLibrary::focusGames(bool first)
{
    if(!m_gridMode){m_list->setFocus();if(!m_list->currentItem()&&m_list->topLevelItemCount())m_list->setCurrentItem(m_list->topLevelItem(0));return;}
    if(m_cards.isEmpty())rebuildGrid();
    if(m_cards.isEmpty()){m_search->setFocus();return;}
    auto* target=m_cards.first();if(!first)for(auto* card:m_cards)if(card->isChecked()){target=card;break;}
    target->setFocus();m_gridScroll->ensureWidgetVisible(target,12,12);
}

void GameLibrary::importFolder()
{
    const auto directory=shellChooseFiles(this,"Import folder",{},false,true).value(0);
    if(!directory.isEmpty())importDirectory(directory);
}
void GameLibrary::importDirectory(const QString& directory)
{
    auto iterator=std::make_shared<QDirIterator>(directory,QDir::Files|QDir::Readable,QDirIterator::Subdirectories);
    auto* scan=new QTimer(this);scan->setInterval(8);
    auto* button=findChild<QPushButton*>("libraryImportFolder");button->setEnabled(false);button->setText("Importing…");
    connect(scan,&QTimer::timeout,this,[this,scan,iterator,button]{
        int visited=0;
        while(iterator->hasNext()&&visited++<32){const auto path=iterator->next();if(QStringList{"iso","gcm","rvz","wia","gcz","wbfs","dol"}.contains(QFileInfo(path).suffix().toLower()))addGame(path);}
        if(!iterator->hasNext()){scan->stop();scan->deleteLater();button->setEnabled(true);button->setText("Import folder");}
    });scan->start();
}
