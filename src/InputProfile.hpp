#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

struct InputPortProfile
{
    QString source = "disabled";
    QString device;
    QMap<QString, QString> values;
};

InputPortProfile defaultInputPortProfile(int port);
QVector<InputPortProfile> loadInputProfiles(const QString& filename);
bool saveInputProfiles(const QString& filename, const QVector<InputPortProfile>& profiles,
                       QString* error = nullptr);
QStringList detectHostInputDevices();
