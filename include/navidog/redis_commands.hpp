#pragma once
#include <QByteArray>
#include <QStringList>
#include <QVector>

namespace navidog {
struct RedisCommand {
    QVector<QByteArray> arguments;
    QString error;
};
RedisCommand parseRedisCommand(const QString&);
QStringList redisCommandNames();
QString quoteRedisArgument(const QByteArray&);
}
