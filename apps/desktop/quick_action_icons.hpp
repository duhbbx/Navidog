#pragma once

#include <QColor>
#include <QIcon>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace navidog::ui {

enum class QuickActionIcon {
    Connection, NewQuery, NewTable, EditTable, Tables, Views,
    Functions, Procedures, Users, Events, Query, Backup, Automation, Model, Bi, Stop
};

inline QIcon quickActionIcon(QuickActionIcon action) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(2);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor blue("#236bb4");
    const QColor teal("#107f78");
    const QColor violet("#7250a9");
    const QColor amber("#935819");
    const QColor slate("#456079");
    const auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) {
        painter.drawLine(QLineF(x1, y1, x2, y2));
    };
    const auto tile = [&](const QColor& accent, const QColor& background) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(1, 1, 30, 30), 8, 8);
        painter.setPen(QPen(accent, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
    };
    const auto badge = [&](const QColor& accent) {
        painter.setPen(QPen(Qt::white, 1.3));
        painter.setBrush(accent);
        painter.drawEllipse(QRectF(19.5, 19.5, 11, 11));
        painter.setPen(QPen(Qt::white, 1.45, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
    };
    const auto plus = [&] {
        line(22, 25, 28, 25);
        line(25, 22, 25, 28);
    };

    switch (action) {
    case QuickActionIcon::Connection:
        tile(teal, QColor("#dcefe9"));
        painter.drawRoundedRect(QRectF(6.5, 10, 7, 12), 2, 2);
        painter.drawRoundedRect(QRectF(18.5, 10, 7, 12), 2, 2);
        line(13.5, 13.5, 18.5, 13.5);
        line(13.5, 18.5, 18.5, 18.5);
        line(14, 7, 18, 7);
        break;
    case QuickActionIcon::NewQuery:
    case QuickActionIcon::Query:
        tile(teal, QColor("#e1f2ed"));
        painter.drawRoundedRect(QRectF(5.5, 7, 21, 18), 3, 3);
        line(10, 12, 14, 16);
        line(14, 16, 10, 20);
        line(17, 20, 22, 20);
        if (action == QuickActionIcon::NewQuery) {
            badge(blue);
            plus();
        }
        break;
    case QuickActionIcon::NewTable:
    case QuickActionIcon::EditTable:
    case QuickActionIcon::Tables:
        tile(blue, QColor("#e2effb"));
        painter.drawRoundedRect(QRectF(5.5, 7, 21, 18), 3, 3);
        line(6, 13, 26, 13);
        line(6, 19, 26, 19);
        line(13, 13, 13, 24);
        if (action == QuickActionIcon::NewTable) {
            badge(teal);
            plus();
        } else if (action == QuickActionIcon::EditTable) {
            badge(amber);
            line(22.5, 27.5, 27.3, 22.7);
        }
        break;
    case QuickActionIcon::Views: {
        tile(blue, QColor("#e2effb"));
        QPainterPath eye(QPointF(5.5, 16));
        eye.cubicTo(11, 7, 21, 7, 26.5, 16);
        eye.cubicTo(21, 25, 11, 25, 5.5, 16);
        painter.drawPath(eye);
        painter.setBrush(blue);
        painter.drawEllipse(QPointF(16, 16), 3, 3);
        break;
    }
    case QuickActionIcon::Functions: {
        tile(violet, QColor("#eee8fa"));
        QPainterPath function(QPointF(19, 7.5));
        function.cubicTo(13.5, 6.5, 14, 12, 12.5, 17);
        function.cubicTo(11.5, 21, 11, 25, 7.5, 24);
        painter.drawPath(function);
        line(9.5, 14, 18, 14);
        line(19.5, 17.5, 25, 23.5);
        line(25, 17.5, 19.5, 23.5);
        break;
    }
    case QuickActionIcon::Procedures:
        tile(amber, QColor("#f7ecdb"));
        painter.drawRoundedRect(QRectF(5, 6, 9, 7), 2, 2);
        painter.drawRoundedRect(QRectF(18, 19, 9, 7), 2, 2);
        line(9.5, 13, 9.5, 22.5);
        line(9.5, 22.5, 18, 22.5);
        line(18.5, 9.5, 26, 9.5);
        line(18.5, 14, 23, 14);
        break;
    case QuickActionIcon::Users:
        tile(violet, QColor("#eee8fa"));
        painter.drawEllipse(QPointF(13, 11), 4, 4);
        painter.drawArc(QRectF(6, 16, 14, 16), 0, 180 * 16);
        painter.drawArc(QRectF(19, 8, 7, 7), -90 * 16, 180 * 16);
        painter.drawArc(QRectF(21, 16, 6, 14), 0, 90 * 16);
        break;
    case QuickActionIcon::Events:
        tile(amber, QColor("#f7ecdb"));
        painter.drawEllipse(QPointF(16, 16), 9, 9);
        line(16, 10.5, 16, 16);
        line(16, 16, 20.5, 18.5);
        break;
    case QuickActionIcon::Backup:
        tile(slate, QColor("#e6edf5"));
        painter.drawRoundedRect(QRectF(6, 15, 20, 10), 2, 2);
        line(6, 19, 12, 19);
        line(20, 19, 26, 19);
        line(16, 6.5, 16, 18.5);
        line(12, 14.5, 16, 18.5);
        line(16, 18.5, 20, 14.5);
        break;
    case QuickActionIcon::Automation:
        tile(teal, QColor("#e1f2ed"));
        painter.drawEllipse(QPointF(16, 16), 9, 9);
        line(16, 9, 13, 16);
        line(13, 16, 19, 16);
        line(19, 16, 16, 23);
        break;
    case QuickActionIcon::Model:
        tile(amber, QColor("#f7ecdb"));
        painter.drawRoundedRect(QRectF(6, 7, 8, 7), 1.5, 1.5);
        painter.drawRoundedRect(QRectF(18, 18, 8, 7), 1.5, 1.5);
        line(10, 14, 10, 22);
        line(10, 22, 18, 22);
        break;
    case QuickActionIcon::Bi:
        tile(violet, QColor("#eee8fa"));
        line(6, 25, 26, 25);
        line(9, 20, 9, 15);
        line(16, 20, 16, 8);
        line(23, 20, 23, 12);
        break;
    case QuickActionIcon::Stop:
        tile(QColor("#a64652"), QColor("#fae7e9"));
        painter.drawRoundedRect(QRectF(9, 9, 14, 14), 3, 3);
        break;
    }
    painter.end();
    return QIcon(pixmap);
}

} // namespace navidog::ui
