#include "ShellGamepad.hpp"
#include <QElapsedTimer>
#include <QMap>
#include <QSet>
#include <Qt>
#include <cmath>
#ifdef GEKKOAOT_LAUNCHER_SDL3
#include <SDL3/SDL.h>
#endif
struct ShellGamepad::State
{
    bool initialized = false, navigating = false;
    QString name;
    int direction = 0;
    QElapsedTimer held;
    qint64 nextRepeat = 0;
#ifdef GEKKOAOT_LAUNCHER_SDL3
    QMap<SDL_JoystickID, SDL_Gamepad *> pads;
    QSet<quint64> pressed;
    QSet<SDL_JoystickID> guide;
#endif
};
ShellGamepad::ShellGamepad(QObject *parent) : QObject(parent), m_state(std::make_unique<State>())
{
#ifdef GEKKOAOT_LAUNCHER_SDL3
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    m_state->initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
#endif
    connect(&m_timer, &QTimer::timeout, this, &ShellGamepad::poll);
    m_timer.start(250);
    QTimer::singleShot(0, this, &ShellGamepad::poll);
}
ShellGamepad::~ShellGamepad()
{
#ifdef GEKKOAOT_LAUNCHER_SDL3
    for (auto *pad : m_state->pads)
        SDL_CloseGamepad(pad);
    if (m_state->initialized)
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
#endif
}
void ShellGamepad::setNavigationEnabled(bool enabled)
{
    if (m_state->navigating != enabled)
    {
        m_state->direction = 0;
        m_state->nextRepeat = 0;
#ifdef GEKKOAOT_LAUNCHER_SDL3
        m_state->pressed.clear();
#endif
    }
    m_state->navigating = enabled;
    m_timer.setInterval(enabled ? 16 : (m_state->name.isEmpty() ? 250 : 40));
}
QString ShellGamepad::controllerName() const
{
    return m_state->name;
}
bool ShellGamepad::available() const
{
    return m_state->initialized;
}
void ShellGamepad::poll()
{
#ifdef GEKKOAOT_LAUNCHER_SDL3
    if (!m_state->initialized)
        return;
    // Enumerating is cheap but need not happen at frame rate. Hotplug events
    // drive subsequent discovery; a single initial scan handles existing pads.
    if (m_state->pads.isEmpty())
    {
        int count = 0;
        auto *ids = SDL_GetGamepads(&count);
        for (int i = 0; i < count; ++i)
            if ((qEnvironmentVariableIsEmpty("GEKKOAOT_LAUNCHER_GAMEPAD_NAME") || QString::fromUtf8(SDL_GetGamepadNameForID(ids[i])).contains(qEnvironmentVariable("GEKKOAOT_LAUNCHER_GAMEPAD_NAME"),Qt::CaseInsensitive)))
            if (auto *p = SDL_OpenGamepad(ids[i]))
                m_state->pads.insert(ids[i], p);
        SDL_free(ids);
    }
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED)
        {
            if (!qEnvironmentVariableIsEmpty("GEKKOAOT_LAUNCHER_GAMEPAD_NAME") && !QString::fromUtf8(SDL_GetGamepadNameForID(event.gdevice.which)).contains(qEnvironmentVariable("GEKKOAOT_LAUNCHER_GAMEPAD_NAME"),Qt::CaseInsensitive))continue;
            if (!m_state->pads.contains(event.gdevice.which))
                if (auto *p = SDL_OpenGamepad(event.gdevice.which))
                    m_state->pads.insert(event.gdevice.which, p);
        }
        else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
        {
            const auto id = event.gdevice.which;
            if (auto *p = m_state->pads.take(id))
                SDL_CloseGamepad(p);
            m_state->guide.remove(id);
            for (auto it = m_state->pressed.begin(); it != m_state->pressed.end();)
            {
                if ((*it >> 32) == id)
                    it = m_state->pressed.erase(it);
                else
                    ++it;
            }
        }
        else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP)
        {
            if (!m_state->pads.contains(event.gbutton.which))
                continue;
            if (!SDL_GamepadConnected(m_state->pads.value(event.gbutton.which)))
                continue;
            const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            const auto b = SDL_GamepadButton(event.gbutton.button);
            const quint64 identity = (quint64(event.gbutton.which) << 32) | quint64(b);
            if (b == SDL_GAMEPAD_BUTTON_GUIDE)
            {
                if (down && !m_state->guide.contains(event.gbutton.which)) {
                    m_state->guide.insert(event.gbutton.which);
                    emit homePressed();
                } else if (!down) m_state->guide.remove(event.gbutton.which);
                continue;
            }
            if (!m_state->navigating)
                continue;
            if (down)
            {
                m_state->pressed.insert(identity);
                if (b == SDL_GAMEPAD_BUTTON_START)
                    emit navigationKey(Qt::Key_Menu);
            }
            else if (m_state->pressed.remove(identity))
            {
                // Activate on release: A/Cross cannot leak as a held press into
                // gameplay when the menu resumes or launches the game.
                if (b == SDL_GAMEPAD_BUTTON_SOUTH)
                    emit navigationKey(Qt::Key_Return);
                else if (b == SDL_GAMEPAD_BUTTON_EAST)
                    emit navigationKey(Qt::Key_Escape);
                else if (b == SDL_GAMEPAD_BUTTON_NORTH)
                    emit navigationKey(Qt::Key_Search);
                else if (b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
                    emit navigationKey(Qt::Key_PageUp);
                else if (b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)
                    emit navigationKey(Qt::Key_PageDown);
            }
        }
    }
    QString name;
    if (!m_state->pads.isEmpty())
        name = QString::fromUtf8(SDL_GetGamepadName(m_state->pads.first()));
    if (name != m_state->name)
    {
        m_state->name = name;
        emit controllerChanged(name);
        setNavigationEnabled(m_state->navigating);
    }
    int direction = 0;
    if (m_state->navigating)
        for (auto *pad : m_state->pads)
        {
            const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                      y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))
                direction = Qt::Key_Left;
            else if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))
                direction = Qt::Key_Right;
            else if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))
                direction = Qt::Key_Up;
            else if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))
                direction = Qt::Key_Down;
            else if (qMax(std::abs(x), std::abs(y)) > 20000)
                direction = std::abs(x) > std::abs(y) ? (x > 0 ? Qt::Key_Right : Qt::Key_Left)
                                                      : (y > 0 ? Qt::Key_Down : Qt::Key_Up);
            if (direction)
                break;
        }
    if (direction != m_state->direction)
    {
        m_state->direction = direction;
        m_state->held.restart();
        m_state->nextRepeat = 330;
        if (direction)
            emit navigationKey(direction);
    }
    else if (direction && m_state->held.elapsed() >= m_state->nextRepeat)
    {
        m_state->nextRepeat = m_state->held.elapsed() + 110;
        emit navigationKey(direction);
    }
#endif
}
