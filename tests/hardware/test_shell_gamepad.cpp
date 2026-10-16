#include "../../src/ShellGamepad.hpp"
#include <QCoreApplication>
#include <QThread>
#include <SDL3/SDL.h>
#include <iostream>
#define CHECK(x)                                                                                             \
    do                                                                                                       \
    {                                                                                                        \
        if (!(x))                                                                                            \
        {                                                                                                    \
            std::cerr << __LINE__ << ": " #x " SDL=" << SDL_GetError() << "\n";                              \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    qputenv("GEKKOAOT_LAUNCHER_GAMEPAD_NAME","GekkoAOT launcher virtual pad");
    ShellGamepad monitor;
    CHECK(monitor.available());
    int homes = 0;
    QList<int> keys;
    QObject::connect(&monitor, &ShellGamepad::homePressed, [&] { ++homes; });
    QObject::connect(&monitor, &ShellGamepad::navigationKey, [&](int key) { keys << key; });
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    desc.name = "GekkoAOT launcher virtual pad";
    const auto id = SDL_AttachVirtualJoystick(&desc);
    CHECK(id);
    CHECK(SDL_IsGamepad(id));
    auto *joystick = SDL_OpenJoystick(id);
    CHECK(joystick);
    monitor.poll();
    auto state = [&](int button, bool down)
    {
        SDL_SetJoystickVirtualButton(joystick, button, down);
        SDL_UpdateJoysticks();
        monitor.poll();
    };
    state(SDL_GAMEPAD_BUTTON_SOUTH, true);
    state(SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK(keys.isEmpty());
    state(SDL_GAMEPAD_BUTTON_GUIDE, true);
    CHECK(homes == 1);
    state(SDL_GAMEPAD_BUTTON_GUIDE, false);
    CHECK(homes == 1);
    monitor.setNavigationEnabled(true);
    state(SDL_GAMEPAD_BUTTON_SOUTH, true);
    CHECK(keys.isEmpty());
    state(SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK(keys == QList<int>{Qt::Key_Return});
    state(SDL_GAMEPAD_BUTTON_EAST, true);
    state(SDL_GAMEPAD_BUTTON_EAST, false);
    CHECK(keys.last() == Qt::Key_Escape);
    keys.clear();
    state(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    CHECK(keys == QList<int>{Qt::Key_Right});
    monitor.poll();
    CHECK(keys.size() == 1);
    QThread::msleep(350);
    monitor.poll();
    CHECK(keys.size() == 2);
    state(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    keys.clear();
    CHECK(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, 26000));
    SDL_UpdateJoysticks();
    monitor.poll();
    CHECK(keys == QList<int>{Qt::Key_Down});
    CHECK(SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFTY, 0));
    SDL_UpdateJoysticks();
    monitor.poll();
    monitor.setNavigationEnabled(false);
    keys.clear();
    state(SDL_GAMEPAD_BUTTON_SOUTH, true);
    monitor.setNavigationEnabled(true);
    state(SDL_GAMEPAD_BUTTON_SOUTH, false);
    CHECK(keys.isEmpty());
    state(SDL_GAMEPAD_BUTTON_GUIDE, true);
    const int homesBeforeDisconnect = homes;
    SDL_CloseJoystick(joystick);
    CHECK(SDL_DetachVirtualJoystick(id));
    monitor.poll();
    CHECK(homes == homesBeforeDisconnect);
    std::cout
        << "Background Guide/Home, release gating, focus gating, stick, d-pad repeat and disconnect passed\n";
}
