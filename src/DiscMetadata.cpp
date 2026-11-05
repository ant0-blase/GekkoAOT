#include "DiscMetadata.hpp"
#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringDecoder>

int expand5(int value) { return (value << 3) | (value >> 2); }

int expand4(int value) { return (value << 4) | value; }

QImage decodeGameCubeBanner(const QByteArray &bnr)
{
    constexpr qsizetype textureOffset = 0x20;
    constexpr int width = 96;
    constexpr int height = 32;
    constexpr qsizetype textureBytes = width * height * 2;
    if (bnr.size() < textureOffset + textureBytes || (bnr.left(4) != "BNR1" && bnr.left(4) != "BNR2"))
        return {};

    QImage image(width, height, QImage::Format_RGBA8888);
    const auto *src = reinterpret_cast<const unsigned char *>(bnr.constData() + textureOffset);
    qsizetype pos = 0;
    for (int tileY = 0; tileY < height; tileY += 4)
    {
        for (int tileX = 0; tileX < width; tileX += 4)
        {
            for (int y = 0; y < 4; ++y)
            {
                for (int x = 0; x < 4; ++x)
                {
                    const quint16 value = static_cast<quint16>(src[pos] << 8 | src[pos + 1]);
                    pos += 2;
                    int r, g, b, a;
                    if (value & 0x8000u)
                    {
                        a = 255;
                        r = expand5((value >> 10) & 0x1F);
                        g = expand5((value >> 5) & 0x1F);
                        b = expand5(value & 0x1F);
                    }
                    else
                    {
                        a = ((value >> 12) & 0x7) * 255 / 7;
                        r = expand4((value >> 8) & 0xF);
                        g = expand4((value >> 4) & 0xF);
                        b = expand4(value & 0xF);
                    }
                    image.setPixelColor(tileX + x, tileY + y, QColor(r, g, b, a));
                }
            }
        }
    }
    return image;
}

QJsonObject readDiscMetadata(const QString &directory)
{
    const auto read = [&](const QString &path)
    {
        QFile f(QDir(directory).filePath(path));
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
    };
    const auto boot = read("sys/boot.bin"), banner = read("meta/opening.bnr");
    QJsonObject out;
    if (boot.size() < 0x440)
        return out;
    const QString id = QString::fromLatin1(boot.first(6));
    const auto text = [&](const QByteArray &raw)
    {
        const auto bytes = raw.first(raw.indexOf('\0') < 0 ? raw.size() : raw.indexOf('\0'));
        if (id.size() == 6 && id[3] == 'J')
        {
            QStringDecoder decoder("Shift-JIS");
            if (decoder.isValid())
                return QString(decoder.decode(bytes)).trimmed();
        }
        QStringDecoder decoder("windows-1252");
        return (decoder.isValid()?QString(decoder.decode(bytes)):QString::fromLatin1(bytes)).trimmed();
    };
    out["gameId"] = id;
    out["gameCode"] = id.first(4);
    out["makerCode"] = id.mid(4, 2);
    out["discNumber"] = int(static_cast<unsigned char>(boot[6])) + 1;
    out["revision"] = int(static_cast<unsigned char>(boot[7]));
    out["title"] = text(boot.mid(0x20, 0x3e0));
    const QChar region = id.size() > 3 ? id[3] : QChar{};
    out["region"] = region == 'E'   ? "USA"
                    : region == 'J' ? "Japan"
                    : region == 'P' || region == 'D' || region == 'F' || region == 'S' || region == 'I'
                        ? "Europe"
                        : "Other";
    if ((banner.startsWith("BNR1") || banner.startsWith("BNR2")) && banner.size() >= 0x1960)
    {
        const QString longName = text(banner.mid(0x1860, 64));
        const QString shortName = text(banner.mid(0x1820, 32));
        if (!longName.isEmpty())
            out["title"] = longName;
        else if (!shortName.isEmpty())
            out["title"] = shortName;
        QString publisher = text(banner.mid(0x18a0, 64));
        if (publisher.isEmpty())
            publisher = text(banner.mid(0x1840, 32));
        out["publisher"] = publisher;
        out["comment"] = text(banner.mid(0x18e0, 128));
    }
    return out;
}

QString discMetadataCachePath(const QString &state, const QString &input)
{
    const QFileInfo info(input);
    const QString canonical = info.canonicalFilePath();
    const QString path = canonical.isEmpty() ? info.absoluteFilePath() : canonical;
    return QDir(state).filePath(
        "library/discs/" +
        QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex()));
}
