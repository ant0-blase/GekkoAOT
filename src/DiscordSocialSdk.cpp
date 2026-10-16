#include "DiscordPresence.hpp"

#ifdef GEKKOAOT_DISCORD_SOCIAL_SDK
#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>
#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace {
class SocialSdkBackend final : public DiscordPresenceBackend {
  std::unique_ptr<discordpp::Client> m_client;

public:
  bool available() const override {
    return discordpp::Client::GetVersionMajor() > 1 ||
           (discordpp::Client::GetVersionMajor() == 1 &&
            discordpp::Client::GetVersionMinor() >= 10);
  }
  bool start(quint64 id) override {
#ifdef Q_OS_ANDROID
    // Official standalone Android initialization, using Qt's current Activity.
    // The SDK AAR (including Java classes) must be packaged, not just its .so.
    const auto activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid() ||
        !QNativeInterface::QAndroidApplication::isActivityContext())
      return false;
    QJniEnvironment env;
    const auto sdk =
        env.findClass("com/discord/socialsdk/DiscordSocialSdkInit");
    if (!sdk) {
      env.checkAndClearExceptions();
      return false;
    }
    const auto init = env->GetStaticMethodID(sdk, "setEngineActivity",
                                             "(Landroid/app/Activity;)V");
    if (!init) {
      env.checkAndClearExceptions();
      return false;
    }
    env->CallStaticVoidMethod(sdk, init, activity.object());
    if (env.checkAndClearExceptions())
      return false;
#endif
    m_client = std::make_unique<discordpp::Client>();
    m_client->SetApplicationId(id);
    return true;
  }
  void poll() override { discordpp::RunCallbacks(); }
  void publish(const DiscordActivity &value, Completion completion) override {
    if (!m_client) {
      completion(false);
      return;
    }
    discordpp::Activity activity;
    activity.SetName("GekkoAOT");
    activity.SetType(discordpp::ActivityTypes::Playing);
    activity.SetDetails(value.details.toStdString());
    activity.SetState(value.state.toStdString());
    if (value.startedAt > 0) {
      discordpp::ActivityTimestamps time;
      time.SetStart(value.startedAt);
      activity.SetTimestamps(time);
    }
    m_client->UpdateRichPresence(activity,
                                 [completion = std::move(completion)](
                                     const discordpp::ClientResult &result) {
                                   completion(result.Successful());
                                 });
  }
  void stop() override {
    if (!m_client)
      return;
    m_client->ClearRichPresence();
    discordpp::RunCallbacks();
    m_client.reset();
  }
  ~SocialSdkBackend() override { stop(); }
};
} // namespace
std::unique_ptr<DiscordPresenceBackend> makeDiscordPresenceBackend() {
  return std::make_unique<SocialSdkBackend>();
}
#else
namespace {
class UnavailableBackend final : public DiscordPresenceBackend {
public:
  bool available() const override { return false; }
  bool start(quint64) override { return false; }
  void poll() override {}
  void publish(const DiscordActivity &, Completion completion) override {
    completion(false);
  }
  void stop() override {}
};
} // namespace
std::unique_ptr<DiscordPresenceBackend> makeDiscordPresenceBackend() {
  return std::make_unique<UnavailableBackend>();
}
#endif
