#include "navidog/database.hpp"
#include <QDir>
#include <QFile>
#include <QUuid>
#include <sqlite3.h>
#include <stdexcept>

namespace navidog {
QString ensureDemoDatabase(const QString& directory) {
    if (!QDir().mkpath(directory)) throw std::runtime_error("无法创建示例目录");
    const auto path = QDir(directory).filePath("harbor-demo.db");
    if (QFile::exists(path)) {
        sqlite3* check = nullptr;
        if (sqlite3_open_v2(path.toUtf8().constData(), &check, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (check) sqlite3_close(check);
            throw std::runtime_error("示例文件无法打开，未作任何修改");
        }
        sqlite3_stmt* stmt = nullptr;
        bool owned = sqlite3_prepare_v2(check, "SELECT value FROM harbor_meta WHERE key='owner'", -1, &stmt, nullptr) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW && QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) == "harbor-demo-v1";
        sqlite3_finalize(stmt); sqlite3_close(check);
        if (!owned) throw std::runtime_error("同名文件不是 Navidog 示例库，未作任何修改");
        return path;
    }
    const auto temp = path + "." + QUuid::createUuid().toString(QUuid::Id128) + ".tmp";
    sqlite3* db = nullptr; sqlite3_stmt* insert = nullptr;
    try {
        if (sqlite3_open(temp.toUtf8().constData(), &db) != SQLITE_OK) throw std::runtime_error("无法创建示例库");
        const char* schema = R"(
            PRAGMA journal_mode=DELETE;
            BEGIN;
            CREATE TABLE harbor_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
            INSERT INTO harbor_meta VALUES('owner','harbor-demo-v1');
            CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT NOT NULL, city TEXT, email TEXT);
            WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<240)
            INSERT INTO customers SELECT x, '客户 '||printf('%03d',x),
              CASE x%4 WHEN 0 THEN '武汉' WHEN 1 THEN '杭州' WHEN 2 THEN '上海' ELSE '成都' END,
              'customer'||x||'@example.com' FROM n;
            CREATE TABLE orders(id INTEGER PRIMARY KEY, order_no TEXT NOT NULL, customer_id INTEGER,
                status TEXT, total_cents INTEGER, created_at TEXT, FOREIGN KEY(customer_id) REFERENCES customers(id));
            CREATE INDEX orders_customer ON orders(customer_id);
        )";
        if (sqlite3_exec(db, schema, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
        if (sqlite3_prepare_v2(db, "INSERT INTO orders VALUES(?1,?2,?3,?4,?5,?6)", -1, &insert, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
        for (int i = 1; i <= 100000; ++i) {
            const auto code = QString("HB-%1").arg(i, 6, 10, QChar('0')).toUtf8();
            const auto date = QString("2026-11-%1 10:%2:00").arg(1 + i % 15, 2, 10, QChar('0')).arg(i % 60, 2, 10, QChar('0')).toUtf8();
            sqlite3_bind_int(insert, 1, i); sqlite3_bind_text(insert, 2, code.constData(), code.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int(insert, 3, 1 + i % 240);
            sqlite3_bind_text(insert, 4, i % 7 == 0 ? "pending" : "paid", -1, SQLITE_STATIC);
            sqlite3_bind_int(insert, 5, 1200 + i % 89000); sqlite3_bind_text(insert, 6, date.constData(), date.size(), SQLITE_TRANSIENT);
            if (sqlite3_step(insert) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
            sqlite3_reset(insert);
        }
        sqlite3_finalize(insert); insert = nullptr;
        if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
        sqlite3_close(db); db = nullptr;
        if (!QFile::rename(temp, path)) throw std::runtime_error("示例文件已存在，未覆盖");
        return path;
    } catch (...) {
        if (insert) sqlite3_finalize(insert);
        if (db) sqlite3_close(db);
        QFile::remove(temp); throw;
    }
}
}
