#pragma once
#include <QObject>
#include <QTimer>
#include <memory>
// Owns only SDL's launcher input subsystem. The game stays in its own process;
// Home is monitored in the background, navigation is gated to the visible UI.
class ShellGamepad final : public QObject
{
    Q_OBJECT
  public:
    explicit ShellGamepad(QObject *parent = nullptr);
    ~ShellGamepad() override;
    void setNavigationEnabled(bool enabled);
    QString controllerName() const;
    bool available() const;
    void poll();
  signals:
    void homePressed();
    void navigationKey(int key);
    void controllerChanged(const QString &name);

  private:
    struct State;
    std::unique_ptr<State> m_state;
    QTimer m_timer;
};
