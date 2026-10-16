#include "LivingRoomView.hpp"
#include "GameCard.hpp"
#include "ShellStyle.hpp"
#include "BuildProgress.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QScroller>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <functional>
#ifndef GEKKOAOT_SOURCE_DIR
#define GEKKOAOT_SOURCE_DIR "."
#endif
namespace
{
void cover(QPainter &p, const QRectF &rect, const QImage &image)
{
    if (image.isNull())
        return;
    const qreal factor = qMax(rect.width() / image.width(), rect.height() / image.height());
    const QSizeF size(image.width() * factor, image.height() * factor);
    p.drawImage(QRectF(rect.center() - QPointF(size.width() / 2, size.height() / 2), size), image);
}
QPushButton *button(const QString &text, QWidget *parent, const QString &name = {})
{
    auto *result = new QPushButton(text, parent);
    result->setObjectName(name);
    result->setCursor(Qt::PointingHandCursor);
    result->setFocusPolicy(Qt::StrongFocus);
    static const QMap<QString, QString> icons = {{"shellPlay", "play"},
                                                 {"shellCompile", "cpu"},
                                                 {"shellOpen", "folder-open"},
                                                 {"shellLibrary", "library"},
                                                 {"shellRecent", "clock"},
                                                 {"shellSettings", "settings-2"},
                                                 {"shellQueue", "list-video"},
                                                 {"shellDesktop", "monitor"},
                                                 {"shellGraphics", "sliders-horizontal"},
                                                 {"shellInput", "gamepad-2"},
                                                 {"shellAdvanced", "settings-2"},
                                                 {"shellAppearance", "sparkles"},
                                                 {"shellDiscord", "messages-square"},
                                                 {"shellMemoryCards", "memory-stick"},
                                                 {"shellResume", "play"},
                                                 {"shellStop", "square"},
                                                 {"shellMotion", "wind"},
                                                 {"shellSettingsBack", "arrow-left"}};
    if (icons.contains(name))
        result->setIcon(shellIcon(icons.value(name), name == "shellPlay"));
    result->setIconSize(QSize(20, 20));
    return result;
}
} // namespace
LivingRoomView::LivingRoomView(QWidget *parent) : QWidget(parent)
{
    setObjectName("livingRoom");
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_StyledBackground, false);
    setStyleSheet(shellControlsStyle());
    m_rootLayout = new QVBoxLayout(this);
    m_rootLayout->setContentsMargins(52, 24, 52, 22);
    m_rootLayout->setSpacing(12);
    auto *header = new QHBoxLayout;
    header->setSpacing(24);
    auto* logo=new QLabel;logo->setObjectName("shellProjectLogo");logo->setFixedSize(44,44);header->addWidget(logo);
    auto *brand = new QLabel("GEKKOAOT");
    brand->setFont(shellDisplayFont(27));
    header->addWidget(brand);
    header->addStretch();
    m_navigation = new QFrame;
    m_navigation->setObjectName("glassNavigation");
    auto *nav = new QHBoxLayout(m_navigation);
    nav->setContentsMargins(5, 5, 5, 5);
    nav->setSpacing(2);
    m_libraryButton = button("Library", m_navigation, "shellLibrary");
    m_libraryButton->setCheckable(true);
    m_libraryButton->setChecked(true);
    m_recentButton = button("Recent", m_navigation, "shellRecent");
    m_recentButton->setCheckable(true);
    auto *settings = button("Settings", m_navigation, "shellSettings");
    for (auto *b : {m_libraryButton, m_recentButton, settings})
        nav->addWidget(b);
    header->addWidget(m_navigation);
    header->addStretch();
    m_clock = new QLabel;
    m_clock->setFont(shellDisplayFont(23));
    header->addWidget(m_clock);
    m_rootLayout->addLayout(header);
    refreshBrand();
    auto *hero = new QWidget;
    m_hero = hero;

    m_heroBanner = new QLabel(hero);
    m_heroBanner->setObjectName("shellHeroBanner");
    m_heroBanner->setAlignment(Qt::AlignCenter);
    m_heroBanner->setStyleSheet("background: rgba(255,255,255,8); border: 1px solid rgba(255,255,255,22); border-radius: 22px;");
    hero->setObjectName("shellHero");
    auto *heroLayout = new QVBoxLayout(hero);
    heroLayout->setContentsMargins(0, 20, 0, 12);
    heroLayout->setSpacing(12);
    auto *eyebrow = new QLabel("PICK UP. PLAY ON.");
    eyebrow->setFont(shellDisplayFont(13));
    eyebrow->setProperty("muted", true);
    heroLayout->addStretch();
    heroLayout->addWidget(eyebrow);
    m_title = new QLabel("Your next adventure.");
    m_title->setObjectName("shellGameTitle");
    m_title->setTextFormat(Qt::PlainText);
    m_title->setWordWrap(true);
    m_title->setMaximumWidth(760);
    heroLayout->addWidget(m_title);
    m_meta = new QLabel("Your GameCube collection, at home.");
    m_meta->setProperty("muted", true);
    heroLayout->addWidget(m_meta);
    m_description = new QLabel;
    m_description->setTextFormat(Qt::PlainText);
    m_description->setWordWrap(true);
    m_description->setMaximumWidth(540);
    m_description->setMaximumHeight(44);
    m_description->setProperty("muted", true);
    heroLayout->addWidget(m_description);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(12);
    m_play = button("Play game", hero, "shellPlay");
    m_compile = button("Compile", hero, "shellCompile");
    auto *open = button("Open game…", hero, "shellOpen");
    actions->addWidget(m_play);
    actions->addWidget(m_compile);
    actions->addWidget(open);
    actions->addStretch();
    heroLayout->addLayout(actions);
    heroLayout->addStretch();
    m_rootLayout->addWidget(hero, 1);
    auto *shelfHeader = new QHBoxLayout;
    m_section = new QLabel("YOUR LIBRARY");
    m_section->setFont(shellDisplayFont(18));
    shelfHeader->addWidget(m_section);
    m_count = new QLabel;
    m_count->setProperty("muted", true);
    shelfHeader->addWidget(m_count);
    shelfHeader->addStretch();
    m_search = new QLineEdit;
    m_search->setObjectName("shellSearch");
    m_search->setClearButtonEnabled(true);
    m_search->setPlaceholderText("Search games");
    m_search->setFixedWidth(230);
    shelfHeader->addWidget(m_search);
    m_rootLayout->addLayout(shelfHeader);
    m_shelf = new QScrollArea;
    m_shelf->setObjectName("shellShelf");
    m_shelf->setWidgetResizable(true);
    m_shelf->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_shelf->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_shelf->setFocusPolicy(Qt::NoFocus);
    m_shelf->viewport()->installEventFilter(this);
    QScroller::grabGesture(m_shelf->viewport(), QScroller::TouchGesture);
    m_shelfContent = new QWidget;
    m_cardLayout = new QHBoxLayout(m_shelfContent);
    m_cardLayout->setContentsMargins(0, 0, 0, 0);
    m_cardLayout->setSpacing(6);
    m_cardLayout->addStretch();
    m_shelf->setWidget(m_shelfContent);
    m_rootLayout->addWidget(m_shelf);
    auto *footer = new QHBoxLayout;
    m_device = new QLabel("Keyboard & mouse");
    m_device->setProperty("muted", true);
    footer->addWidget(m_device);
    footer->addStretch();
    m_status = new QLabel("A / ×  Select     B / ○  Back     Home  Menu");
    m_status->setObjectName("shellStatus");
    m_status->setProperty("muted", true);
    footer->addWidget(m_status);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setMinimumWidth(180);
    footer->addStretch();
    m_queue = button("Compilations", this, "shellQueue");
    footer->addWidget(m_queue);
    auto *desktop = button("Desktop", this, "shellDesktop");
    footer->addWidget(desktop);
    m_rootLayout->addLayout(footer);
    m_settings = new QFrame(this);
    m_settings->setObjectName("glassSettings");
    m_settings->hide();
    auto *panel = new QVBoxLayout(m_settings);
    panel->setContentsMargins(24, 24, 24, 24);
    panel->setSpacing(8);
    auto addSetting = [&](const QString &title, const QString &name, auto callback)
    {
        auto *b = button(title, m_settings, name);
        b->setFixedHeight(52);
        b->setStyleSheet("QPushButton {text-align:left;padding:0 12px;background:transparent;border:1px solid transparent;border-radius:11px;}QPushButton:hover {background:rgba(255,255,255,15);}QPushButton:focus {background:rgba(255,255,255,18);border-color:rgba(255,255,255,100);}QPushButton:pressed {background:rgba(255,255,255,26);}");
        panel->addWidget(b);
        m_settingsButtons << b;
        connect(b, &QPushButton::clicked, this, callback);
        return b;
    };
    m_resume = addSetting("Resume game", "shellResume",
                          [this]
                          {
                              closeSettings();
                              emit resumeRequested();
                          });
    m_stop = addSetting("Stop game", "shellStop",
                        [this]
                        {
                            closeSettings();
                            emit stopRequested();
                        });
    addSetting("Graphics", "shellGraphics", [this] { emit graphicsRequested(); });
    addSetting("Controllers", "shellInput", [this] { emit inputRequested(); });
    addSetting("Memory cards", "shellMemoryCards", [this] { emit memoryCardsRequested(); });
    addSetting("Advanced", "shellAdvanced", [this] { emit advancedRequested(); });
    addSetting("Appearance", "shellAppearance", [this] { emit appearanceRequested(); });
    addSetting("Discord Rich Presence", "shellDiscord", [this] { emit discordRequested(); });
    m_motion = addSetting("Motion: on", "shellMotion",
                          [this]
                          {
                              setReducedMotion(!m_reducedMotion);
                              emit reducedMotionChanged(m_reducedMotion);
                          });
    addSetting("Back", "shellSettingsBack", [this] { closeSettings(); });
    connect(m_libraryButton, &QPushButton::clicked, this,
            [this]
            {
                m_recentOnly = false;
                m_libraryButton->setChecked(true);
                m_recentButton->setChecked(false);
                rebuildShelf();if(!m_reducedMotion)shellRevealSnapshot(m_shelf);
            });
    connect(m_recentButton, &QPushButton::clicked, this,
            [this]
            {
                m_recentOnly = true;
                m_recentButton->setChecked(true);
                m_libraryButton->setChecked(false);
                rebuildShelf();if(!m_reducedMotion)shellRevealSnapshot(m_shelf);
            });
    connect(settings, &QPushButton::clicked, this, [this] {
        m_settings->isVisible() ? closeSettings() : openSettings();
    });
    connect(m_play, &QPushButton::clicked, this, &LivingRoomView::activateCurrent);
    connect(m_compile, &QPushButton::clicked, this,
            [this]
            {
                if (!m_current.path.isEmpty())
                    emit compileRequested(m_current.path);
            });
    connect(open, &QPushButton::clicked, this, &LivingRoomView::openRequested);
    connect(desktop, &QPushButton::clicked, this, &LivingRoomView::desktopRequested);
    connect(m_queue, &QPushButton::clicked, this, &LivingRoomView::queueRequested);
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuildShelf(); });
    auto *clock = new QTimer(this);
    clock->setInterval(1000);
    auto tick = [this] { m_clock->setText(QTime::currentTime().toString("HH:mm")); };
    connect(clock, &QTimer::timeout, this, tick);
    clock->start();
    tick();
    m_backdropMotion.setDuration(320);
    m_backdropMotion.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_backdropMotion, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v)
            {
                m_blend = v.toReal();
                update();
            });
    m_scrollMotion.setDuration(230);
    m_scrollMotion.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_scrollMotion, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &v) { m_shelf->horizontalScrollBar()->setValue(v.toInt()); });
    shellShadow(m_settings,24);
    shellAnimateButtons(this);
    setGameRunning(false);
    rebuildShelf();
    qApp->installEventFilter(this);
}
void LivingRoomView::setGames(const QList<LibraryEntry> &games)
{
    m_games = games;
    rebuildShelf();
}
void LivingRoomView::rebuildShelf()
{
    QList<LibraryEntry> visible;
    const QString filter=m_search->text().trimmed();
    for(const auto& game:m_games)if((!m_recentOnly || game.lastPlayed) && (filter.isEmpty() || (game.title+game.gameId+game.publisher).contains(filter,Qt::CaseInsensitive)))visible<<game;
    if(m_recentOnly)std::stable_sort(visible.begin(),visible.end(),[](const auto& a,const auto& b){return a.lastPlayed>b.lastPlayed;});
    bool same=visible.size()==m_cards.size();
    for(int i=0;same && i<visible.size();++i)same=m_cards[i]->game.path==visible[i].path && m_cards[i]->game.title==visible[i].title && m_cards[i]->game.banner==visible[i].banner;
    if(same){for(const auto& game:visible)if(game.path==m_current.path)selectGame(game);return;}
    const int previousScroll=m_shelf->horizontalScrollBar()->value();
    const bool shelfHadFocus = dynamic_cast<GameCard *>(QApplication::focusWidget());
    for (auto *c : m_cards)
    {
        m_cardLayout->removeWidget(c);
        c->hide();
        c->deleteLater();
    }
    m_cards.clear();
    const QString query = m_search->text().trimmed();
    for (const auto &game : visible)
    {
        auto *card = new GameCard(game, m_shelfContent);
        card->setReducedMotion(m_reducedMotion);
        card->setFixedSize(qBound(220, (width() - 100) / 4, 340), qBound(180, height() / 4, 230));
        card->highlighted = [this, game] { selectGame(game); };
        connect(card, &QAbstractButton::clicked, this,
                [this, game]
                {
                    selectGame(game);
                    activateCurrent();
                });
        m_cards << card;
        m_cardLayout->insertWidget(m_cardLayout->count() - 1, card);
        card->show();
    }
    m_cardLayout->activate();
    m_count->setText(QString("%1 game%2").arg(m_cards.size()).arg(m_cards.size() == 1 ? "" : "s"));
    m_section->setText(m_recentOnly ? "RECENTLY PLAYED" : "YOUR LIBRARY");
    if (m_cards.isEmpty())
    {
        m_current = {};
        m_title->setText(query.isEmpty() ? "Make room for play." : "No games found.");
        m_meta->setText(query.isEmpty() ? "Open a game to start your collection."
                                        : "Try a different title, Game ID or publisher.");
        m_description->clear();
        m_play->setEnabled(false);
        m_compile->setEnabled(false);
        updateBackdrop({});
    }
    else
    {
        int chosen = 0;
        for (int i = 0; i < m_cards.size(); ++i)
            if (m_cards[i]->game.path == m_current.path)
                chosen = i;
        selectGame(m_cards[chosen]->game);
        if (shelfHadFocus)focusCard(chosen);
        else m_shelf->horizontalScrollBar()->setValue(previousScroll);
    }
}
QStringList LivingRoomView::visibleGames() const
{
    QStringList out;
    for (auto *c : m_cards)
        out << c->game.path;
    return out;
}
void LivingRoomView::setCurrentGame(const QString &path)
{
    for (const auto &game : m_games)
        if (game.path == path)
        {
            selectGame(game);
            return;
        }
}
void LivingRoomView::selectGame(const LibraryEntry &game)
{
    const bool changed = m_current.path != game.path || m_current.banner.cacheKey() != game.banner.cacheKey();
    m_current = game;
    m_title->setText(game.title);
    const QString played =
        game.playSeconds
            ? QString("%1h %2m played").arg(game.playSeconds / 3600).arg(game.playSeconds / 60 % 60)
            : "Ready to play";
    m_meta->setText(
        QString("GAMECUBE   /   %1   /   %2").arg(game.region.isEmpty() ? game.gameId : game.region, played));
    m_description->setText(game.publisher);
    m_play->setEnabled(!m_buildBusy && QFileInfo::exists(game.path));
    m_play->setText(m_running ? (game.path == m_runningPath ? "Resume game" : "Switch game") : "Play game");
    m_compile->setEnabled(QFileInfo::exists(game.path));
    if (changed)
        updateBackdrop(game);
}
void LivingRoomView::updateBackdrop(const LibraryEntry &game)
{
    m_previousBackdrop = m_backdrop;
    m_backdrop = QImage(1280,720,QImage::Format_RGB32);
    const QColor accent=bannerAccent(game.banner);
    QPainter atmosphere(&m_backdrop);
    atmosphere.fillRect(m_backdrop.rect(),QColor("#242424"));
    QRadialGradient glow(QPointF(1100,180),1000);
    glow.setColorAt(0,accent);glow.setColorAt(1,QColor("#202020"));
    atmosphere.fillRect(m_backdrop.rect(),glow);atmosphere.end();
    m_heroBanner->setPixmap(QPixmap::fromImage(game.banner).scaled(384,128,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    m_heroBanner->setVisible(!game.banner.isNull() && width()>=1100);
    m_backdropMotion.stop();
    if (m_reducedMotion || !isVisible())
    {
        m_blend = 1;
        update();
    }
    else
    {
        m_backdropMotion.setStartValue(0.0);
        m_backdropMotion.setEndValue(1.0);
        m_backdropMotion.start();
    }
}
void LivingRoomView::setGameRunning(bool running, const QString &path)
{
    m_running = running;
    m_runningPath = path;
    m_resume->setVisible(running);
    m_stop->setVisible(running);
    if (running)
        m_buildBusy = false;
    findChild<QPushButton *>("shellOpen")->setEnabled(!m_running && !m_buildBusy);
    if (!m_current.path.isEmpty())
        selectGame(m_current);
}
void LivingRoomView::setBuildStatus(const QString &text, int progress)
{
    m_buildBusy = progress >= 0;
    if (!m_current.path.isEmpty())
        selectGame(m_current);
    findChild<QPushButton *>("shellOpen")->setEnabled(!m_running && !m_buildBusy);
    m_status->setText(text.isEmpty() ? "A / ×  Select     B / ○  Back     Home  Menu"
                                     : buildProgressLabel(text) + (progress >= 0 ? QString(" · %1 %").arg(progress) : QString{}));
}
void LivingRoomView::setQueueCount(int count)
{
    m_queue->setText(count ? QString("Compilations · %1").arg(count) : "Compilations");
}
void LivingRoomView::setControllerName(const QString &name)
{
    m_device->setText(name.isEmpty() ? "Keyboard & mouse" : name);
}
void LivingRoomView::setReducedMotion(bool reduced)
{
    m_reducedMotion = reduced;
    m_settings->setProperty("shellReducedMotion",reduced);
    m_motion->setText(reduced ? "Motion: reduced" : "Motion: on");
    if (reduced)
    {
        m_backdropMotion.stop();
        m_scrollMotion.stop();
        m_blend = 1;
        update();
    }
    for (auto *c : m_cards)
        c->setReducedMotion(reduced);
}
void LivingRoomView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(rect(), QColor("#212121"));
    auto layer = [&](const QImage &image, qreal opacity)
    {
        p.save();
        p.setOpacity(opacity);
        cover(p, rect(), image);
        p.restore();
    };
    if (m_blend < 1)
        layer(m_previousBackdrop, .38 * (1 - m_blend));
    layer(m_backdrop, .38 * m_blend);
    QLinearGradient side(0, 0, width(), 0);
    side.setColorAt(0, QColor(25, 25, 25, 240));
    side.setColorAt(.5, QColor(25, 25, 25, 115));
    side.setColorAt(1, QColor(28, 28, 28, 75));
    p.fillRect(rect(), side);
    QLinearGradient shade(0, 0, 0, height());
    shade.setColorAt(0, QColor(35, 35, 35, 100));
    shade.setColorAt(.5, QColor(29, 29, 29, 25));
    shade.setColorAt(1, QColor(23, 23, 23, 250));
    p.fillRect(rect(), shade);
    // A quiet dot texture echoes the display type without introducing colour.
    p.setPen(QColor(255, 255, 255, 9));
    for (int y = 12; y < height(); y += 24)
        for (int x = 12; x < width(); x += 24)
            p.drawPoint(x, y);
}
void LivingRoomView::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    const bool compact = width() < 1100;
    const int margin = compact ? 30 : 52;
    m_rootLayout->setContentsMargins(margin, 24, margin, 18);
    QFont title = font();
    title.setPixelSize(qBound(28, height() / 18, 52));
    title.setWeight(QFont::DemiBold);
    m_title->setFont(title);
    m_title->setMaximumWidth(qMin(int(width() * .53), 760));
    QTimer::singleShot(0,this,[this]{m_heroBanner->setGeometry(m_hero->width()-440,qMax(8,(m_hero->height()-168)/2),420,168);});
    m_heroBanner->setVisible(!m_current.banner.isNull() && width()>=1100);
    m_shelf->setFixedHeight(qBound(180, height() / 4, 230));
    for (auto *c : m_cards)
        c->setFixedSize(qBound(220, (width() - 100) / 4, 340), m_shelf->height());
    const int panelWidth=320;
    const QRect resting(width()-margin-panelWidth,96,panelWidth,qMin(m_settings->layout()->sizeHint().height(),height()-120));
    m_settings->setProperty("shellRestingGeometry",resting);
    m_settings->setGeometry(resting);
    m_device->setVisible(width() >= 1100);
    m_search->setFixedWidth(compact ? 180 : 230);
    m_description->setVisible(height() >= 760);
}
void LivingRoomView::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    QTimer::singleShot(0, this,
                       [this]
                       {
                           if (isVisible())
                               m_play->isEnabled() ? m_play->setFocus() : m_libraryButton->setFocus();
                       });
}
void LivingRoomView::focusCard(int index)
{
    if (m_cards.isEmpty())
        return;
    index = qBound(0, index, int(m_cards.size()) - 1);
    auto *c = m_cards[index];
    c->setFocus(Qt::TabFocusReason);
    const int target = qBound(m_shelf->horizontalScrollBar()->minimum(),
                              c->x() - (m_shelf->viewport()->width() - c->width()) / 2,
                              m_shelf->horizontalScrollBar()->maximum());
    m_scrollMotion.stop();
    if (m_reducedMotion)
        m_shelf->horizontalScrollBar()->setValue(target);
    else
    {
        m_scrollMotion.setStartValue(m_shelf->horizontalScrollBar()->value());
        m_scrollMotion.setEndValue(target);
        m_scrollMotion.start();
    }
}
void LivingRoomView::openSettings()
{
    const int margin = width()<1100 ? 30 : 52;
    const QRect resting(width()-margin-352,96,352,qMin(m_settings->layout()->sizeHint().height(),height()-120));
    m_settings->setProperty("shellRestingGeometry",resting);
    m_settings->setGeometry(resting);
    m_settings->setProperty("shellExitCaptured",false);m_settings->show();
    m_settings->raise();
    auto *source=findChild<QWidget*>("shellSettings");
    shellRevealFrom(m_settings,source->mapToGlobal(source->rect().center()));
    for (auto *b : m_settingsButtons)
        if (b->isVisible() && b->isEnabled())
        {
            b->setFocus();
            break;
        }
}
void LivingRoomView::closeSettings()
{
    if(m_settings->isVisible())shellRetire(m_settings);
    m_settings->hide();
    m_play->setFocus();
}
void LivingRoomView::activateCurrent()
{
    if (m_running && m_current.path == m_runningPath)
        emit resumeRequested();
    else if (!m_current.path.isEmpty() && m_play->isEnabled())
        emit playRequested(m_current.path);
}
void LivingRoomView::navigate(int key)
{
    auto *focus = QApplication::focusWidget();
    if (key == Qt::Key_Menu)
    {
        openSettings();
        return;
    }
    if (key == Qt::Key_Search)
    {
        m_search->setFocus();
        return;
    }
    if (key == Qt::Key_PageUp)
    {
        m_libraryButton->click();
        m_libraryButton->setFocus();
        return;
    }
    if (key == Qt::Key_PageDown)
    {
        m_recentButton->click();
        m_recentButton->setFocus();
        return;
    }
    if (key == Qt::Key_Escape)
    {
        if (m_settings->isVisible())
            closeSettings();
        else if (m_search->hasFocus() && !m_search->text().isEmpty())
            m_search->clear();
        else
            emit backRequested();
        return;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter)
    {
        if (auto *b = qobject_cast<QAbstractButton *>(focus))
        {
            b->click();
            return;
        }
        if (focus == m_search)
        {
            focusCard(0);
            return;
        }
    }
    if (m_settings->isVisible())
    {
        QList<QAbstractButton *> visible;
        for (auto *b : m_settingsButtons)
            if (b->isVisible() && b->isEnabled())
                visible << b;
        int index = visible.indexOf(qobject_cast<QAbstractButton *>(focus));
        if ((key == Qt::Key_Up || key == Qt::Key_Down) && !visible.isEmpty())
            visible[(index + (key == Qt::Key_Down ? 1 : visible.size() - 1)) % visible.size()]->setFocus();
        return;
    }
    int cardIndex = -1;
    for (int i = 0; i < m_cards.size(); ++i)
        if (m_cards[i] == focus)
            cardIndex = i;
    if (cardIndex >= 0)
    {
        if (key == Qt::Key_Left || key == Qt::Key_Right)
            focusCard(cardIndex + (key == Qt::Key_Right ? 1 : -1));
        else if (key == Qt::Key_Up)
            m_search->setFocus();
        else if (key == Qt::Key_Down)
            m_queue->setFocus();
        return;
    }
    if (focus == m_search)
    {
        if (key == Qt::Key_Down)
            focusCard(0);
        else if(key == Qt::Key_Up)
            m_play->setFocus();
        else if(key == Qt::Key_Left)
            m_play->setFocus();
        return;
    }
    if (key == Qt::Key_Down &&
        (focus == m_play || focus == m_compile || focus == findChild<QPushButton *>("shellOpen")))
    {
        m_search->setFocus();
        return;
    }
    if (key == Qt::Key_Up && (focus == m_queue || focus == findChild<QPushButton *>("shellDesktop")))
    {
        focusCard(0);
        return;
    }
    const QList<QAbstractButton *> top = {m_libraryButton, m_recentButton,
                                          findChild<QPushButton *>("shellSettings")};
    const QList<QAbstractButton *> hero = {m_play, m_compile, findChild<QPushButton *>("shellOpen")};
    const QList<QAbstractButton *> footer = {m_queue, findChild<QPushButton *>("shellDesktop")};
    for (const auto &row : {top, hero, footer})
    {
        int index = row.indexOf(qobject_cast<QAbstractButton *>(focus));
        if (index < 0)
            continue;
        if (key == Qt::Key_Left || key == Qt::Key_Right)
        {
            for (int n = 1; n <= row.size(); ++n)
            {
                auto *b = row[(index + (key == Qt::Key_Right ? n : row.size() * 2 - n)) % row.size()];
                if (b->isEnabled())
                {
                    b->setFocus();
                    break;
                }
            }
        }
        else if (key == Qt::Key_Down)
        {
            if (row == top)
                m_play->isEnabled() ? m_play->setFocus() : m_search->setFocus();
        }
        else if (key == Qt::Key_Up && row == hero)
            m_libraryButton->setFocus();
        return;
    }
}
void LivingRoomView::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right || e->key() == Qt::Key_Up ||
        e->key() == Qt::Key_Down || e->key() == Qt::Key_Return || e->key() == Qt::Key_Escape)
    {
        navigate(e->key());
        e->accept();
    }
    else
        QWidget::keyPressEvent(e);
}
bool LivingRoomView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress && isVisible() && m_settings->isVisible() &&
        !QApplication::activePopupWidget() && !shellActiveDialog(window())) {
        auto *target = qobject_cast<QWidget *>(watched);
        auto *mouse = static_cast<QMouseEvent *>(event);
        const auto global = mouse->globalPosition().toPoint();
        if (target && target->window() == window() && mouse->button() == Qt::LeftButton &&
            rect().contains(mapFromGlobal(global)) && !m_settings->rect().contains(m_settings->mapFromGlobal(global))) {
            closeSettings();
            return true;
        }
    }
    if (watched == m_shelf->viewport() && event->type() == QEvent::Wheel)
    {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const QPoint delta = wheel->pixelDelta().isNull() ? wheel->angleDelta() : wheel->pixelDelta();
        auto *bar = m_shelf->horizontalScrollBar();
        const int movement = delta.x() ? delta.x() : delta.y();
        const int target = qBound(bar->minimum(), bar->value() - movement, bar->maximum());
        m_scrollMotion.stop();
        if (m_reducedMotion)
            bar->setValue(target);
        else
        {
            m_scrollMotion.setStartValue(bar->value());
            m_scrollMotion.setEndValue(target);
            m_scrollMotion.start();
        }
        wheel->accept();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void LivingRoomView::refreshBrand()
{
    auto* logo=findChild<QLabel*>("shellProjectLogo");
    logo->setPixmap(shellProjectIcon().pixmap(44,44));
    logo->setVisible(QSettings("GekkoAOT","GekkoAOT").value("ui/showProjectLogo",true).toBool());
}
