#include "InputProfile.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>

namespace
{
QString cleaned(QString value)
{
    value = value.trimmed();
    value.remove('\r');
    value.remove('\n');
    return value;
}

void setDigital(InputPortProfile& p, const QString& name, const QString& key,
                const QString& button, int joyButton)
{
    p.values[name + "_key"] = key;
    p.values[name + "_button"] = button;
    p.values[name + "_joy_button"] = QString::number(joyButton);
}

void setAxis(InputPortProfile& p, const QString& name, const QString& neg, const QString& pos,
             const QString& axis, int joyAxis, bool invert, int deadzone = 4096)
{
    p.values[name + "_negative_key"] = neg;
    p.values[name + "_positive_key"] = pos;
    p.values[name + "_axis"] = axis;
    p.values[name + "_joy_axis"] = QString::number(joyAxis);
    p.values[name + "_invert"] = invert ? "true" : "false";
    p.values[name + "_deadzone"] = QString::number(deadzone);
}

void setTrigger(InputPortProfile& p, const QString& name, const QString& key,
                const QString& axis, int joyAxis)
{
    p.values[name + "_key"] = key;
    p.values[name + "_axis"] = axis;
    p.values[name + "_joy_axis"] = QString::number(joyAxis);
}
}

InputPortProfile defaultInputPortProfile(int port)
{
    InputPortProfile p;
    p.source = port == 0 ? "auto" : "disabled";
    setDigital(p, "a", "J", "south", 0);
    setDigital(p, "b", "K", "east", 1);
    setDigital(p, "x", "U", "west", 2);
    setDigital(p, "y", "I", "north", 3);
    setDigital(p, "start", "Return", "start", 9);
    setDigital(p, "z", "R", "right_shoulder", 5);
    setDigital(p, "l", "Q", "left_shoulder", 4);
    setDigital(p, "r", "E", "right_shoulder", 5);
    setDigital(p, "dpad_up", "T", "dpad_up", 12);
    setDigital(p, "dpad_down", "G", "dpad_down", 13);
    setDigital(p, "dpad_left", "F", "dpad_left", 14);
    setDigital(p, "dpad_right", "H", "dpad_right", 15);
    setAxis(p, "main_x", "A", "D", "leftx", 0, false);
    setAxis(p, "main_y", "S", "W", "lefty", 1, true);
    setAxis(p, "c_x", "Left", "Right", "rightx", 2, false);
    setAxis(p, "c_y", "Down", "Up", "righty", 3, true);
    setTrigger(p, "l_analog", "Q", "left_trigger", 4);
    setTrigger(p, "r_analog", "E", "right_trigger", 5);
    return p;
}

QVector<InputPortProfile> loadInputProfiles(const QString& filename)
{
    QVector<InputPortProfile> result;
    for (int i = 0; i < 4; ++i)
        result.append(defaultInputPortProfile(i));

    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;

    int current = -1;
    QTextStream in(&file);
    while (!in.atEnd())
    {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
            continue;
        if (line.startsWith('[') && line.endsWith(']'))
        {
            const QString section = line.mid(1, line.size() - 2).trimmed().toLower();
            current = -1;
            for (int i = 0; i < 4; ++i)
                if (section == QString("port%1").arg(i + 1))
                    current = i;
            continue;
        }
        if (current < 0)
            continue;
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed().toLower();
        const QString value = cleaned(line.mid(eq + 1));
        if (key == "source")
            result[current].source = value.toLower();
        else if (key == "device")
            result[current].device = value;
        else
            result[current].values[key] = value;
    }
    return result;
}

bool saveInputProfiles(const QString& filename, const QVector<InputPortProfile>& profiles,
                       QString* error)
{
    const QFileInfo info(filename);
    if (!QDir().mkpath(info.absolutePath()))
    {
        if (error) *error = "cannot create input profile directory: " + info.absolutePath();
        return false;
    }
    QSaveFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        if (error) *error = "cannot write " + filename;
        return false;
    }
    QTextStream out(&file);
    out << "# GekkoAOT native SDL3 input profile\n"
           "# source = auto | keyboard | gamepad | joystick | disabled\n";
    for (int i = 0; i < 4; ++i)
    {
        const InputPortProfile p = i < profiles.size() ? profiles[i] : defaultInputPortProfile(i);
        out << "\n[port" << (i + 1) << "]\n";
        out << "source=" << cleaned(p.source) << "\n";
        out << "device=" << cleaned(p.device) << "\n";
        for (auto it = p.values.cbegin(); it != p.values.cend(); ++it)
            out << it.key() << '=' << cleaned(it.value()) << "\n";
    }
    if (!file.commit())
    {
        if (error) *error = "cannot commit " + filename;
        return false;
    }
    return true;
}

QStringList detectHostInputDevices()
{
    QStringList devices;
#ifdef Q_OS_LINUX
    QFile file("/proc/bus/input/devices");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        QTextStream in(&file);
        QString name;
        QString handlers;
        const auto flush = [&] {
            if (!name.isEmpty() &&
                (handlers.contains("js") || handlers.contains("event") ||
                 handlers.contains("kbd")))
                devices.append(name);
            name.clear();
            handlers.clear();
        };
        while (!in.atEnd())
        {
            const QString line = in.readLine();
            if (line.trimmed().isEmpty())
            {
                flush();
                continue;
            }
            if (line.startsWith("N: Name="))
            {
                name = line.mid(QString("N: Name=").size()).trimmed();
                if (name.startsWith('"') && name.endsWith('"'))
                    name = name.mid(1, name.size() - 2);
            }
            else if (line.startsWith("H: Handlers="))
                handlers = line.mid(QString("H: Handlers=").size()).trimmed();
        }
        flush();
    }
#endif
    devices.removeDuplicates();
    devices.sort(Qt::CaseInsensitive);
    return devices;
}
