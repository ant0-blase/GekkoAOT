#include "DiscordPresence.hpp"
#include <QPointer>
#include <QRegularExpression>

#ifndef GEKKOAOT_DISCORD_APPLICATION_ID
#define GEKKOAOT_DISCORD_APPLICATION_ID ""
#endif

namespace {
QString displayText(QString text) {
  // Discord's text fields accept at most 128 UTF-8 bytes. Never split a
  // codepoint or forward line breaks/control characters from disc metadata.
  text.remove(QRegularExpression(
      QStringLiteral("[\\x{0000}-\\x{001f}\\x{007f}-\\x{009f}]")));
  text = text.simplified();
  while (text.toUtf8().size() > 128) {
    const bool pair = text.size() > 1 && text.back().isLowSurrogate();
    text.chop(pair ? 2 : 1);
  }
  return text;
}
} // namespace

DiscordActivity discordActivityFor(const DiscordPresenceContext &c) {
  if (c.gameRunning)
    return {displayText(c.gameTitle.isEmpty() ? QStringLiteral("Playing a game")
                                              : c.gameTitle),
            c.paused ? QStringLiteral("Paused")
                     : QStringLiteral("Playing with GekkoAOT"),
            c.gameStartedAt};
  if (c.compiling)
    return {displayText(c.compilationTitle.isEmpty()
                            ? QStringLiteral("Preparing a game")
                            : c.compilationTitle),
            QStringLiteral("Compiling with GekkoAOT"), 0};
  return {QStringLiteral("Browsing the library"),
          c.tvMode ? QStringLiteral("TV Mode") : QStringLiteral("Desktop"), 0};
}

DiscordPresence::DiscordPresence(
    QObject *parent, std::unique_ptr<DiscordPresenceBackend> backend,
    Clock clock)
    : QObject(parent), m_backend(std::move(backend)), m_now(std::move(clock)) {
  m_clock.start();
  m_activity = discordActivityFor({});
  m_timer.setInterval(50);
  connect(&m_timer, &QTimer::timeout, this, &DiscordPresence::tick);
}
DiscordPresence::~DiscordPresence() { shutdown(); }

bool DiscordPresence::validApplicationId(const QString &value) {
  const auto id = value.trimmed();
  if (!QRegularExpression(QStringLiteral("^[1-9][0-9]{0,19}$"))
           .match(id)
           .hasMatch())
    return false;
  bool ok = false;
  return id.toULongLong(&ok) != 0 && ok;
}
QString DiscordPresence::defaultApplicationId() {
  const QString env =
      qEnvironmentVariable("GEKKOAOT_DISCORD_APPLICATION_ID").trimmed();
  return env.isEmpty() ? QStringLiteral(GEKKOAOT_DISCORD_APPLICATION_ID) : env;
}
void DiscordPresence::setStatus(Status value) {
  if (m_status != value) {
    m_status = value;
    emit statusChanged();
  }
}
QString DiscordPresence::statusText() const {
  switch (m_status) {
  case Status::Disabled:
    return tr("Discord Rich Presence is off.");
  case Status::MissingApplicationId:
    return tr("Enter a valid Discord Application ID.");
  case Status::Unavailable:
    return tr("This build does not include the Discord Social SDK.");
  case Status::Connecting:
    return tr("Connecting to Discord…");
  case Status::Active:
    return tr("Activity shared with Discord.");
  case Status::WaitingForDiscord:
    return tr("Waiting for Discord. Open Discord and sign in; reconnection is "
              "automatic.");
  }
  return {};
}
void DiscordPresence::configure(bool enabled, const QString &id) {
  const quint64 value = validApplicationId(id) ? id.trimmed().toULongLong() : 0;
  if (enabled && m_started && value == m_applicationId)
    return;
  shutdown();
  if (!enabled)
    return;
  if (!value) {
    setStatus(Status::MissingApplicationId);
    return;
  }
  if (!m_backend->available()) {
    setStatus(Status::Unavailable);
    return;
  }
  m_applicationId = value;
  m_started = m_backend->start(value);
  if (!m_started) {
    setStatus(Status::Unavailable);
    return;
  }
  setStatus(Status::Connecting);
  m_timer.start();
  tick();
}
void DiscordPresence::shutdown() {
  ++m_generation;
  m_timer.stop();
  if (m_started)
    m_backend->stop();
  m_started = m_pending = m_haveSent = false;
  m_applicationId = 0;
  m_nextSend = 0;
  m_retrySeconds = 30;
  setStatus(Status::Disabled);
}
void DiscordPresence::setActivity(const DiscordActivity &activity) {
  const DiscordActivity clean{displayText(activity.details),
                              displayText(activity.state), activity.startedAt};
  if (m_activity == clean)
    return;
  m_activity = clean;
  if (m_haveSent && m_status == Status::Active)
    m_nextSend = qMin(m_nextSend, m_lastSentAt + 15000);
  if (m_started)
    tick();
}
void DiscordPresence::tick() {
  if (!m_started)
    return;
  m_backend->poll();
  const qint64 now = this->now();
  if (m_pending && now - m_pendingSince > 15000) {
    // Ignore a delayed callback from this request, and retry the latest state.
    ++m_request;
    m_pending = false;
    m_nextSend = now + 30000;
    setStatus(Status::WaitingForDiscord);
  }
  if (m_pending || now < m_nextSend)
    return;
  m_pending = true;
  m_pendingSince = now;
  const auto activity = m_activity;
  const quint64 generation = m_generation, request = ++m_request;
  QPointer<DiscordPresence> self(this);
  m_backend->publish(activity, [self, generation, request,
                                activity](bool success) {
    if (!self || self->m_generation != generation || self->m_request != request)
      return;
    self->m_pending = false;
    const qint64 now = self->now();
    if (success) {
      self->m_haveSent = true;
      self->m_lastSentAt = now;
      self->m_retrySeconds = 30;
      // Coalesce rapid UI changes, and refresh unchanged activity periodically
      // so Discord starting/restarting later recovers without a game restart.
      self->m_nextSend = now + (self->m_activity == activity ? 60000 : 15000);
      self->setStatus(Status::Active);
    } else {
      self->m_nextSend = now + self->m_retrySeconds * 1000;
      self->m_retrySeconds = qMin(120, self->m_retrySeconds * 2);
      self->setStatus(Status::WaitingForDiscord);
    }
  });
}
