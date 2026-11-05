#include "../../src/DiscordPresence.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
#include <vector>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " #x "\n";                                   \
      return 1;                                                                \
    }                                                                          \
  } while (false)

class FakeDiscord final : public DiscordPresenceBackend {
public:
  bool supported = true;
  int starts = 0, stops = 0, polls = 0;
  quint64 id = 0;
  std::vector<DiscordActivity> sent;
  std::vector<Completion> callbacks;
  bool available() const override { return supported; }
  bool start(quint64 value) override {
    ++starts;
    id = value;
    return true;
  }
  void poll() override { ++polls; }
  void publish(const DiscordActivity &activity,
               Completion completion) override {
    sent.push_back(activity);
    callbacks.push_back(std::move(completion));
  }
  void stop() override { ++stops; }
};

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  // Explicit manual smoke test only: publishes library presence to the local
  // signed-in Discord client, then clears it. CTest never selects this mode.
  if (app.arguments().contains("--live")) {
    DiscordPresence live;
    if (!live.available()) {
      std::cerr << "Social SDK unavailable\n";
      return 1;
    }
    QObject::connect(&live, &DiscordPresence::statusChanged, &app, [&] {
      std::cout << live.statusText().toStdString() << '\n' << std::flush;
      if (live.status() == DiscordPresence::Status::Active)
        QTimer::singleShot(1000, &app, [&] {
          live.shutdown();
          app.exit(0);
        });
    });
    QTimer::singleShot(20000, &app, [&] {
      live.shutdown();
      app.exit(1);
    });
    live.setActivity(discordActivityFor({}));
    live.configure(true, DiscordPresence::defaultApplicationId());
    return app.exec();
  }
  for (const auto &value :
       {"", "0", "-1", "1.2", "123abc", "18446744073709551616"})
    CHECK(!DiscordPresence::validApplicationId(value));
  CHECK(DiscordPresence::validApplicationId(" 123456789012345678 "));
  CHECK(DiscordPresence::validApplicationId("18446744073709551615"));
  DiscordPresenceContext context;
  CHECK(discordActivityFor(context).details == "Browsing the library");
  context.tvMode = true;
  CHECK(discordActivityFor(context).state == "TV Mode");
  context.compiling = true;
  context.compilationTitle = "Luigi's Mansion";
  CHECK(discordActivityFor(context).state == "Compiling with GekkoAOT");
  context.gameRunning = true;
  context.gameTitle = "Tales of Symphonia";
  context.gameStartedAt = 1700000000;
  CHECK(discordActivityFor(context).details == context.gameTitle);
  context.paused = true;
  CHECK(discordActivityFor(context).state == "Paused" &&
        discordActivityFor(context).startedAt == 1700000000);
  context.tvMode = false;
  context.paused = false;
  CHECK(discordActivityFor(context).startedAt == 1700000000);
  context.gameTitle = QString(120, QChar(0x00e9)) + QString::fromUtf8("🎮\n");
  CHECK(discordActivityFor(context).details.toUtf8().size() <= 128);
  CHECK(!discordActivityFor(context).details.contains('\n'));

  qint64 clock = 1000;
  auto backend = std::make_unique<FakeDiscord>();
  auto *fake = backend.get();
  DiscordPresence presence(nullptr, std::move(backend), [&] { return clock; });
  presence.configure(true, "invalid");
  CHECK(presence.status() == DiscordPresence::Status::MissingApplicationId &&
        fake->starts == 0);
  presence.setActivity(
      {"Luigi's Mansion", "Playing with GekkoAOT", 1700000000});
  presence.configure(true, "123456789012345678");
  CHECK(fake->starts == 1 && fake->id == 123456789012345678ULL &&
        fake->sent.size() == 1);
  auto first = fake->callbacks.back();
  first(true);
  CHECK(presence.status() == DiscordPresence::Status::Active);
  presence.setActivity({"Luigi's Mansion", "Paused", 1700000000});
  presence.setActivity(
      {"Luigi's Mansion", "Playing with GekkoAOT", 1700000000});
  CHECK(fake->sent.size() == 1); // Coalesce rapid pause/resume and UI changes.
  // The presence timer fires every 50 ms; Windows timers have ~15.6 ms
  // granularity, so wait for expected changes instead of a fixed window.
  auto waitFor = [&](auto done) {
    QElapsedTimer wait;
    wait.start();
    while (!done() && wait.elapsed() < 2000) {
      app.processEvents();
      QThread::msleep(1);
    }
  };
  auto tick = [&] {
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 200) {
      app.processEvents();
      QThread::msleep(1);
    }
  };
  clock += 15001;
  waitFor([&] { return fake->sent.size() == 2; });
  CHECK(fake->sent.size() == 2 &&
        fake->sent.back().state == "Playing with GekkoAOT");
  fake->callbacks.back()(false);
  CHECK(presence.status() == DiscordPresence::Status::WaitingForDiscord);
  presence.setActivity({"Browsing the library", "Desktop", 0});
  clock += 29999;
  tick();
  CHECK(fake->sent.size() == 2);
  clock += 2;
  waitFor([&] { return fake->sent.size() == 3; });
  CHECK(fake->sent.size() == 3 && fake->sent.back().startedAt == 0);
  fake->callbacks.back()(true);
  clock += 60001;
  waitFor([&] { return fake->sent.size() == 4; });
  CHECK(fake->sent.size() == 4); // Reconnect/heartbeat with Discord restarted.
  const auto late = fake->callbacks.back();
  presence.configure(false, "123456789012345678");
  CHECK(fake->stops == 1 &&
        presence.status() == DiscordPresence::Status::Disabled);
  late(true);
  CHECK(presence.status() == DiscordPresence::Status::Disabled);
  presence.configure(true, "123456789012345679");
  CHECK(fake->id == 123456789012345679ULL && fake->starts == 2);
  const auto stale = fake->callbacks.back();
  clock += 15001;
  waitFor([&] {
    return presence.status() == DiscordPresence::Status::WaitingForDiscord;
  });
  CHECK(presence.status() == DiscordPresence::Status::WaitingForDiscord);
  stale(true);
  CHECK(presence.status() == DiscordPresence::Status::WaitingForDiscord);
  presence.shutdown();
  fake->supported = false;
  presence.configure(true, "123456789012345678");
  CHECK(presence.status() == DiscordPresence::Status::Unavailable &&
        fake->starts == 2);
  std::cout << "Discord activity, lifecycle, throttling and recovery passed\n";
}
