#pragma once

#include "BuildController.hpp"

#include <QMainWindow>
#include <QMap>
#include <QString>
#include <QStringList>

class QImage;

class QAction;
class QCheckBox;
class QComboBox;
class QDialog;
class QDragEnterEvent;
class QEvent;
class QDropEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedLayout;
class QToolBar;
class QToolButton;
class QWidget;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildUi();
    void buildMenusAndToolbar();
    void connectUi();
    void loadSettings();
    void saveSettings() const;
    void browseIso();
    void browseLlvmDir();
    void setIsoPath(const QString& path, bool autoPlay = true);
    void updateGameMetadata();
    void updateGameEnhancementsMenu();
    QMap<QString, QString> gameEnhancementEnvironment() const;
    void gameEnhancementChanged(const QString& label);
    void updateStatusFromOutput(const QString& text);
    void launchAction(const QString& action);
    void setBusy(bool busy);
    void setAdvancedVisible(bool visible);
    void setLogVisible(bool visible);
    void showGraphicsConfig();
    void showInputConfig();
    void showLoader(const QString& title, const QString& status, int progress);
    void showGameViewport();
    void showIdleViewport(const QString& message = {});
    void toggleGameViewFullscreen();
    void setGameViewFullscreen(bool fullscreen);
    void updateLoaderBanner();
    QString frontendUserDir() const;
    qreal effectiveDisplayScale() const;
    QString displaySessionDescription() const;
    bool writeFrontendConfig(QString* error = nullptr) const;
    BuildRequest requestFor(const QString& action) const;
    static QString slugForPath(const QString& path);
    static QString discIdForPath(const QString& path);
    static QString gameTitleForPath(const QString& path);
    static bool looksLikeDiscImage(const QString& path);
    static QImage gameCubeBannerForPath(const QString& path);

    BuildController m_controller;
    QString m_isoPath;
    QString m_activeAction;
    bool m_runtimeStarted = false;
    bool m_stopRequested = false;
    bool m_gameViewFullscreen = false;
    bool m_wasMaximizedBeforeGameFullscreen = false;
    bool m_restoreAdvancedAfterGameFullscreen = false;
    QString m_progressStage;
    QString m_progressSubstage;
    QString m_progressBuffer;
    QString m_secondaryKind;
    QString m_secondaryName;
    int m_secondaryIndex = 0;
    int m_secondaryTotal = 0;
    int m_lastProgress = 0;

    QString m_graphicsBackend = "Vulkan";
    QString m_resolution = "1920x1080";
    QString m_windowSystem = "auto";
    QString m_aspectMode = "4:3";
    bool m_fullscreen = false;
    bool m_showFps = true;
    int m_fpsLimit = 120;
    QStringList m_controllers;

    QWidget* m_viewFrame = nullptr;
    QWidget* m_gameViewport = nullptr;
    QWidget* m_loaderOverlay = nullptr;
    QStackedLayout* m_viewStack = nullptr;
    QLabel* m_bannerLabel = nullptr;
    QLabel* m_loaderTitle = nullptr;
    QLabel* m_loaderStatus = nullptr;
    QLabel* m_loaderHint = nullptr;
    QProgressBar* m_progress = nullptr;

    QDialog* m_advancedDialog = nullptr;
    QToolBar* m_mainToolbar = nullptr;
    QMenu* m_gameEnhancementsMenu = nullptr;
    QToolButton* m_gameEnhancementsButton = nullptr;
    QWidget* m_advancedPanel = nullptr;
    QWidget* m_logPanel = nullptr;
    QCheckBox* m_showLogCheck = nullptr;

    QComboBox* m_backend = nullptr;
    QComboBox* m_profile = nullptr;
    QComboBox* m_toolchain = nullptr;
    QComboBox* m_nativeAbi = nullptr;
    QLineEdit* m_llvmDir = nullptr;
    QPushButton* m_llvmBrowse = nullptr;
    QSpinBox* m_jobs = nullptr;
    QLineEdit* m_runArgs = nullptr;

    QPushButton* m_pgo = nullptr;
    QPushButton* m_inspect = nullptr;
    QPushButton* m_compile = nullptr;
    QPushButton* m_buildTools = nullptr;
    QPushButton* m_clean = nullptr;
    QPlainTextEdit* m_log = nullptr;

    QAction* m_openAction = nullptr;
    QAction* m_playAction = nullptr;
    QAction* m_stopAction = nullptr;
    QAction* m_graphicsAction = nullptr;
    QAction* m_inputAction = nullptr;
    QAction* m_advancedAction = nullptr;
    QAction* m_analyzeAction = nullptr;
    QAction* m_compileAction = nullptr;
    QAction* m_pgoAction = nullptr;
    QAction* m_buildToolsAction = nullptr;
    QAction* m_cleanAction = nullptr;
    QAction* m_fullscreenAction = nullptr;
};
