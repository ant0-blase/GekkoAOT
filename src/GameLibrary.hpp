#pragma once
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QWidget>
#include <QImage>
#include <functional>
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QLabel;
class QTimer;
class QStackedWidget;
class QScrollArea;
class QGridLayout;
class QPushButton;
class GameCard;
struct LibraryEntry {
    QString path, title, gameId, publisher, region, comment;
    QImage banner;
    qint64 lastPlayed = 0, playSeconds = 0;
};
class GameLibrary final : public QWidget
{
  public:
    GameLibrary(const QString &stateDirectory, const QString &reader, QWidget *parent = nullptr);
    void addGame(const QString &path);
    void importCachedGames();
    void focusGames(bool first = false);
    void toggleView();
    void importFolder();
    void importDirectory(const QString& path);
    bool navigateGames(int key);
    void selectGame(const QString &path);
    void recordLaunch(const QString &path);
    void addPlayTime(const QString &path, qint64 seconds);
    QJsonObject metadata(const QString &path) const;
    QString selectedPath() const;
    QStringList selectedPaths() const;
    QList<LibraryEntry> entries() const;
    std::function<void()> changed;
    std::function<void(const QStringList&)> compileRequested;
    std::function<void(const QString &)> selected;
    std::function<void(const QString &)> activated;
    std::function<void(const QString &)> error;
    std::function<void(const QString &)> metadataReady;

  private:
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject*,QEvent*) override;
    QString m_root, m_reader, m_activePath, m_staging;
    QMap<QString, QJsonObject> m_games;
    QStringList m_pending;
    bool m_legacyImported = false;
    QProcess m_process;
    QTimer *m_timeout = nullptr;
    QTreeWidget *m_list = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_count = nullptr;
    QStackedWidget* m_views = nullptr;
    QScrollArea* m_gridScroll = nullptr;
    QWidget* m_gridContent = nullptr;
    QGridLayout* m_gridLayout = nullptr;
    QTimer* m_gridRefresh = nullptr;
    QList<GameCard*> m_cards;
    bool m_gridMode = true;
    int m_gridColumns = 1;
    void rebuildGrid();
    void reflowGrid();
    void syncCardSelection();
    void contextMenu(const QString& path,const QPoint& global);
    void load();
    void save();
    void refreshRow(const QString &path);
    void filter();
    void readNext();
    void finishRead(int code, QProcess::ExitStatus status);
    qint64 m_sourceSize = 0, m_sourceMtime = 0;
    void properties(const QString &path);
    QString cachePath(const QString &path) const;
};
