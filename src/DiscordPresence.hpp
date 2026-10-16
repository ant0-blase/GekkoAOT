#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>

// Public display data only. Disc paths, command lines and logs never enter
// this model. Presentation changes must not restart a running game's clock.
struct DiscordActivity {
  QString details, state;
  qint64 startedAt = 0;
  bool operator==(const DiscordActivity &) const = default;
};

struct DiscordPresenceContext {
  QString gameTitle, compilationTitle;
  qint64 gameStartedAt = 0;
  bool gameRunning = false, paused = false, compiling = false, tvMode = false;
};

DiscordActivity discordActivityFor(const DiscordPresenceContext &context);

// The transport is shared by desktop and Android. An injected transport lets
// tests exercise disconnects and late callbacks without publishing activity.
class DiscordPresenceBackend {
public:
  using Completion = std::function<void(bool)>;
  virtual ~DiscordPresenceBackend() = default;
  virtual bool available() const = 0;
  virtual bool start(quint64 applicationId) = 0;
  virtual void poll() = 0;
  virtual void publish(const DiscordActivity &, Completion) = 0;
  virtual void stop() = 0;
};
std::unique_ptr<DiscordPresenceBackend> makeDiscordPresenceBackend();

class DiscordPresence final : public QObject {
  Q_OBJECT
public:
  using Clock = std::function<qint64()>;
  enum class Status {
    Disabled,
    MissingApplicationId,
    Unavailable,
    Connecting,
    Active,
    WaitingForDiscord
  };
  explicit DiscordPresence(QObject *parent = nullptr,
                           std::unique_ptr<DiscordPresenceBackend> backend =
                               makeDiscordPresenceBackend(),
                           Clock clock = {});
  ~DiscordPresence() override;
  void configure(bool enabled, const QString &applicationId);
  void setActivity(const DiscordActivity &activity);
  void shutdown();
  bool available() const { return m_backend->available(); }
  Status status() const { return m_status; }
  QString statusText() const;
  static bool validApplicationId(const QString &id);
  static QString defaultApplicationId();
signals:
  void statusChanged();

private:
  void tick();
  void setStatus(Status status);
  qint64 now() const { return m_now ? m_now() : m_clock.elapsed(); }
  std::unique_ptr<DiscordPresenceBackend> m_backend;
  QTimer m_timer;
  QElapsedTimer m_clock;
  Clock m_now;
  DiscordActivity m_activity;
  Status m_status = Status::Disabled;
  quint64 m_applicationId = 0, m_generation = 0, m_request = 0;
  bool m_started = false, m_pending = false, m_haveSent = false;
  qint64 m_nextSend = 0, m_pendingSince = 0, m_lastSentAt = 0;
  int m_retrySeconds = 30;
};
