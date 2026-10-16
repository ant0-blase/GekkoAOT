#pragma once
#include "GameLibrary.hpp"
#include <QVariantAnimation>
#include <QWidget>
class QAbstractButton;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QHBoxLayout;
class QVBoxLayout;
class GameCard;
class LivingRoomView final : public QWidget
{
    Q_OBJECT
  public:
    explicit LivingRoomView(QWidget *parent = nullptr);
    void setGames(const QList<LibraryEntry> &games);
    void setCurrentGame(const QString &path);
    QString currentGame() const
    {
        return m_current.path;
    }
    QStringList visibleGames() const;
    void setGameRunning(bool running, const QString &path = {});
    void setBuildStatus(const QString &text, int progress = -1);
    void setQueueCount(int count);
    void setControllerName(const QString &name);
    void setReducedMotion(bool reduced);
    bool reducedMotion() const
    {
        return m_reducedMotion;
    }
    void navigate(int key);
    void openSettings();
    void refreshBrand();
  signals:
    void playRequested(const QString &path);
    void compileRequested(const QString &path);
    void resumeRequested();
    void stopRequested();
    void desktopRequested();
    void openRequested();
    void graphicsRequested();
    void inputRequested();
    void advancedRequested();
    void appearanceRequested();
    void discordRequested();
    void memoryCardsRequested();
    void queueRequested();
    void backRequested();
    void reducedMotionChanged(bool reduced);

  protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private:
    QList<LibraryEntry> m_games;
    QList<GameCard *> m_cards;
    LibraryEntry m_current;
    QString m_runningPath;
    bool m_running = false, m_buildBusy = false, m_recentOnly = false, m_reducedMotion = false;
    QImage m_backdrop, m_previousBackdrop;
    qreal m_blend = 1;
    QVariantAnimation m_backdropMotion, m_scrollMotion;
    QFrame *m_navigation;
    QFrame *m_settings;
    QLabel* m_heroBanner;
    QWidget* m_hero;
    QLabel *m_clock, *m_title, *m_meta, *m_description, *m_section, *m_count, *m_status, *m_device;
    QPushButton *m_libraryButton, *m_recentButton, *m_play, *m_compile, *m_resume, *m_stop, *m_motion,
        *m_queue;
    QLineEdit *m_search;
    QScrollArea *m_shelf;
    QWidget *m_shelfContent;
    QHBoxLayout *m_cardLayout;
    QVBoxLayout *m_rootLayout;
    QList<QAbstractButton *> m_settingsButtons;
    void rebuildShelf();
    void selectGame(const LibraryEntry &game);
    void updateBackdrop(const LibraryEntry &game);
    void focusCard(int index);
    void closeSettings();
    void activateCurrent();
};
