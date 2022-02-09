// This file Copyright © 2012-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>

#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

#include "FaviconCache.h"

/***
****
***/

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
Q_NETWORK_EXPORT bool qIsEffectiveTLD(QStringView domain);
#endif

namespace
{

QString getTopLevelDomain(QUrl const& url)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)

    auto const host = url.host();
    auto const dot = QChar(QLatin1Char('.'));

    for (auto dot_pos = host.indexOf(dot); dot_pos != -1; dot_pos = host.indexOf(dot, dot_pos + 1))
    {
        if (qIsEffectiveTLD(QStringView(&host.data()[dot_pos + 1], host.size() - dot_pos - 1)))
        {
            return host.mid(dot_pos);
        }
    }

    return {};

#else

    return url.topLevelDomain();

#endif
}

} // namespace

/***
****
***/

FaviconCache::FaviconCache()
    : nam_(new QNetworkAccessManager(this))
{
    connect(nam_, &QNetworkAccessManager::finished, this, &FaviconCache::onRequestFinished);
}

/***
****
***/

namespace
{

QPixmap scale(QPixmap const& pixmap)
{
    return pixmap.scaled(FaviconCache::getIconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QString getCacheDir()
{
    auto const base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(base).absoluteFilePath(QStringLiteral("favicons"));
}

QString getScrapedFile()
{
    auto const base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return QDir(base).absoluteFilePath(QStringLiteral("favicons-scraped.txt"));
}

void markUrlAsScraped(QString const& url_str)
{
    auto skip_file = QFile(getScrapedFile());
    if (skip_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Append))
    {
        skip_file.write(url_str.toUtf8());
        skip_file.write("\n");
    }
}

} // namespace

void FaviconCache::ensureCacheDirHasBeenScanned()
{
    static bool has_been_scanned = false;
    if (has_been_scanned)
    {
        return;
    }

    has_been_scanned = true;

    // remember which hosts we've asked for a favicon so that we
    // don't re-ask them every time we start a new session
    if (auto skip_file = QFile(getScrapedFile()); skip_file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        while (!skip_file.atEnd())
        {
            auto const url = QString::fromUtf8(skip_file.readLine()).trimmed();
            auto const key = getKey(QUrl{ url });
            keys_.insert({ url, key });
            pixmaps_.try_emplace(key);
        }
    }

    // load the cached favicons
    auto cache_dir = QDir(getCacheDir());
    cache_dir.mkpath(cache_dir.absolutePath());
    for (auto const& file : cache_dir.entryList(QDir::Files | QDir::Readable))
    {
        QPixmap pixmap(cache_dir.absoluteFilePath(file));
        if (!pixmap.isNull())
        {
            pixmaps_[file] = scale(pixmap);
        }
    }
}

/***
****
***/

QString FaviconCache::getDisplayName(Key const& key)
{
    auto name = key;
    if (!name.isEmpty())
    {
        name.front() = name.front().toTitleCase();
    }
    return name;
}

FaviconCache::Key FaviconCache::getKey(QUrl const& url)
{
    auto host = url.host();

    // remove tld
    auto const suffix = getTopLevelDomain(url);
    host.truncate(host.size() - suffix.size());

    // remove subdomain
    auto const pos = host.indexOf(QLatin1Char('.'));
    return pos < 0 ? host : host.remove(0, pos + 1);
}

FaviconCache::Key FaviconCache::getKey(QString const& displayName)
{
    return displayName.toLower();
}

QSize FaviconCache::getIconSize()
{
    return { 16, 16 };
}

QPixmap FaviconCache::find(Key const& key)
{
    ensureCacheDirHasBeenScanned();

    return pixmaps_[key];
}

FaviconCache::Key FaviconCache::add(QString const& url_str)
{
    ensureCacheDirHasBeenScanned();

    // find or add this url's key
    if (auto k_it = keys_.find(url_str); k_it != keys_.end())
    {
        return k_it->second;
    }

    auto const url = QUrl{ url_str };
    auto const key = getKey(url);
    keys_.insert({ url_str, key });

    // Try to download a favicon if we don't have one.
    // Add a placeholder to prevent repeat downloads.
    if (pixmaps_.try_emplace(key).second)
    {
        markUrlAsScraped(url_str);

        auto const scrape = [this](auto const host)
        {
            auto const schemes = std::array<QString, 2>{
                QStringLiteral("http"),
                QStringLiteral("https"),
            };
            auto const suffixes = std::array<QString, 5>{
                QStringLiteral("gif"), //
                QStringLiteral("ico"), //
                QStringLiteral("jpg"), //
                QStringLiteral("png"), //
                QStringLiteral("svg"), //
            };
            for (auto const& scheme : schemes)
            {
                for (auto const& suffix : suffixes)
                {
                    auto const path = QStringLiteral("%1://%2/favicon.%3").arg(scheme).arg(host).arg(suffix);
                    nam_->get(QNetworkRequest(path));
                }
            }
        };

        // tracker.domain.com
        auto host = url.host();
        scrape(host);

        auto const delim = QStringLiteral(".");
        auto const has_subdomain = host.count(delim) > 1;
        if (has_subdomain)
        {
            auto const original_subdomain = host.left(host.indexOf(delim));
            host.remove(0, original_subdomain.size() + 1);
            // domain.com
            scrape(host);

            auto const www = QStringLiteral("www");
            if (original_subdomain != www)
            {
                // www.domain.com
                scrape(QStringLiteral("%1.%2").arg(www).arg(host));
            }
        }
    }

    return key;
}

void FaviconCache::onRequestFinished(QNetworkReply* reply)
{
    auto const key = getKey(reply->url());

    QPixmap pixmap;

    QByteArray const content = reply->readAll();

    if (reply->error() == QNetworkReply::NoError)
    {
        pixmap.loadFromData(content);
    }

    if (!pixmap.isNull())
    {
        // save it in memory...
        pixmaps_[key] = scale(pixmap);

        // save it on disk...
        QDir cache_dir(getCacheDir());
        cache_dir.mkpath(cache_dir.absolutePath());
        QFile file(cache_dir.absoluteFilePath(key));
        file.open(QIODevice::WriteOnly);
        file.write(content);
        file.close();

        // notify listeners
        emit pixmapReady(key);
    }

    reply->deleteLater();
}
