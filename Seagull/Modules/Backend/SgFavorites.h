#pragma once

#include <QObject>
#include <QHash>

// Singleton that persists a set of favorited YouTube channels to
// Config/favorites.json. Keyed by channelUrl; stores the display name too.
//
// Access via SgFavorites::instance(). The first call creates the singleton
// and loads the JSON from disk. Every toggle writes back immediately.
class SgFavorites : public QObject {
    Q_OBJECT
public:
    static SgFavorites* instance();

    bool isFavorited(const QString& channelUrl) const;
    void setFavorited(const QString& channelUrl, const QString& channelName, bool favorited);
    void toggle(const QString& channelUrl, const QString& channelName);

signals:
    void changed(const QString& channelUrl, bool isFavorited);

private:
    explicit SgFavorites(QObject* parent = nullptr);

    void load();
    void save() const;

    // url -> display name. Presence in the map means favorited.
    QHash<QString, QString> m_favorites;
};
