#pragma once
#include <QImage>
#include <QJsonObject>
#include <QString>
QImage decodeGameCubeBanner(const QByteArray &bnr);
QJsonObject readDiscMetadata(const QString &cacheDirectory);

QString discMetadataCachePath(const QString &stateDirectory, const QString &path);
