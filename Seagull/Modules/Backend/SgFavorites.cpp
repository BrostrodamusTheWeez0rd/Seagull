#include "SgFavorites.h"
#include "SgPaths.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

SgFavorites* SgFavorites::instance() {
    static SgFavorites* s_instance = new SgFavorites(nullptr);
    return s_instance;
}

SgFavorites::SgFavorites(QObject* parent) : QObject(parent) {
    load();
}

bool SgFavorites::isFavorited(const QString& channelUrl) const {
    return m_favorites.contains(channelUrl);
}

void SgFavorites::setFavorited(const QString& channelUrl, const QString& channelName, bool favorited) {
    if (favorited) {
        if (m_favorites.value(channelUrl) == channelName && m_favorites.contains(channelUrl))
            return; // already stored with the same name — no-op
        m_favorites.insert(channelUrl, channelName);
    } else {
        if (!m_favorites.contains(channelUrl))
            return; // not present — no-op
        m_favorites.remove(channelUrl);
    }
    save();
    emit changed(channelUrl, favorited);
}

void SgFavorites::toggle(const QString& channelUrl, const QString& channelName) {
    setFavorited(channelUrl, channelName, !isFavorited(channelUrl));
}

void SgFavorites::load() {
    const QString path = SgPaths::configDir() + "/favorites.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return; // no file yet — start empty

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) return;

    for (const QJsonValue& v : doc.array()) {
        const QJsonObject obj = v.toObject();
        const QString url  = obj.value("url").toString();
        const QString name = obj.value("name").toString();
        if (!url.isEmpty())
            m_favorites.insert(url, name);
    }
}

void SgFavorites::save() const {
    const QString dir  = SgPaths::configDir();
    const QString path = dir + "/favorites.json";

    QDir().mkpath(dir); // ensure Config/ exists (matches SgPaths::configFile() callers)

    QJsonArray arr;
    for (auto it = m_favorites.constBegin(); it != m_favorites.constEnd(); ++it) {
        QJsonObject obj;
        obj["url"]  = it.key();
        obj["name"] = it.value();
        arr.append(obj);
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
}
