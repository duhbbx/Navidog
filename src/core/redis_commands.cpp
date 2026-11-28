#include "navidog/redis_commands.hpp"

namespace navidog {
RedisCommand parseRedisCommand(const QString& line) {
    RedisCommand command;
    const auto text = line.toUtf8();
    const auto hex = [](char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    const auto space = [](char value) { return value == ' ' || value == '\t' || value == '\r'; };
    for (int i = 0; i < text.size();) {
        if (space(text[i])) { ++i; continue; }
        if (command.arguments.isEmpty() && text[i] == '#') break;
        QByteArray argument;
        char quote = 0;
        while (i < text.size()) {
            const char value = text[i++];
            if (!quote && space(value)) break;
            if (value == quote) {
                quote = 0;
                if (i < text.size() && !space(text[i])) command.error = "引号结束后需要空格";
                break;
            }
            if (!quote && (value == '"' || value == '\'')) { quote = value; continue; }
            if (value != '\\' || !quote) { argument += value; continue; }
            if (i == text.size()) break;
            const char escaped = text[i++];
            if (quote == '\'') {
                if (escaped != '\'') argument += '\\';
                argument += escaped;
            } else if (escaped == 'x' && i + 1 < text.size() && hex(text[i]) >= 0 && hex(text[i + 1]) >= 0) {
                argument += char(hex(text[i]) * 16 + hex(text[i + 1]));
                i += 2;
            } else {
                switch (escaped) {
                case 'n': argument += '\n'; break;
                case 'r': argument += '\r'; break;
                case 't': argument += '\t'; break;
                case 'b': argument += '\b'; break;
                case 'a': argument += '\a'; break;
                default: argument += escaped; break;
                }
            }
        }
        if (quote) command.error = "命令参数的引号未闭合";
        if (!command.error.isEmpty()) break;
        command.arguments << argument;
    }
    if (command.error.isEmpty() && !command.arguments.isEmpty()) {
        struct Arity { const char* name; int min; int max; };
        static constexpr Arity common[] = {
            {"PING", 0, 1}, {"GET", 1, 1}, {"TYPE", 1, 1}, {"TTL", 1, 1},
            {"SET", 2, -1}, {"DEL", 1, -1}, {"EXISTS", 1, -1}, {"HGET", 2, 2},
            {"LRANGE", 3, 3}, {"SADD", 2, -1}, {"DBSIZE", 0, 0},
        };
        const auto name = command.arguments.first().toUpper();
        const auto count = command.arguments.size() - 1;
        for (const auto& arity : common)
            if (name == arity.name && (count < arity.min || (arity.max >= 0 && count > arity.max))) {
                command.error = "Redis 命令参数数量不正确";
                break;
            }
    }
    return command;
}

QStringList redisCommandNames() {
    return {"APPEND", "DBSIZE", "DECR", "DEL", "EXISTS", "EXPIRE", "GET", "GETDEL",
            "HDEL", "HEXISTS", "HGET", "HGETALL", "HKEYS", "HLEN", "HSCAN", "HSET",
            "INCR", "INFO", "LINDEX", "LLEN", "LPOP", "LPUSH", "LRANGE", "MGET",
            "MSET", "PERSIST", "PING", "PTTL", "RENAME", "RPOP", "RPUSH", "SADD",
            "SCAN", "SCARD", "SET", "SISMEMBER", "SMEMBERS", "SREM", "SSCAN", "STRLEN",
            "TTL", "TYPE", "UNLINK", "XADD", "XLEN", "XRANGE", "ZADD", "ZCARD", "ZRANGE",
            "ZREM", "ZSCAN", "ZSCORE"};
}

QString quoteRedisArgument(const QByteArray& value) {
    QByteArray quoted(1, '"');
    for (const unsigned char byte : value) {
        if (byte < 32 || byte > 126) quoted += "\\x" + QByteArray(1, char(byte)).toHex();
        else {
            if (byte == '"' || byte == '\\') quoted += '\\';
            quoted += char(byte);
        }
    }
    return QString::fromLatin1(quoted + '"');
}
}
