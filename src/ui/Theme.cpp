// SPDX-License-Identifier: MIT
#include "Theme.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QSvgRenderer>

namespace aslgc::theme {

namespace {

// Renders a `currentColor` SVG in the palette colour matching the icon mode.
class TintedSvgEngine : public QIconEngine {
public:
    TintedSvgEngine(QByteArray svg, size_t hash) : m_svg(std::move(svg)), m_hash(hash) {}

    QIconEngine* clone() const override { return new TintedSvgEngine(*this); }
    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { return size; }
    QString key() const override { return QStringLiteral("aslgc-tinted-svg"); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, dpr));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override {
        const QColor color = colorFor(mode);
        const QSize px = (QSizeF(size) * scale).toSize();
        if (px.isEmpty()) return {};
        const QString cacheKey =
            QStringLiteral("aslgc-icon:%1:%2x%3:%4").arg(m_hash).arg(px.width()).arg(px.height()).arg(color.rgba());
        QPixmap pm;
        if (!QPixmapCache::find(cacheKey, &pm)) {
            QByteArray svg = m_svg;
            svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
            if (qMin(size.width(), size.height()) <= 16) svg.replace("stroke-width=\"2\"", "stroke-width=\"1.75\"");
            QSvgRenderer renderer(svg);
            pm = QPixmap(px);
            pm.fill(Qt::transparent);
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(px)));
            p.end();
            QPixmapCache::insert(cacheKey, pm);
        }
        pm.setDevicePixelRatio(scale);
        return pm;
    }

private:
    static QColor colorFor(QIcon::Mode mode) {
        const QPalette pal = QApplication::palette();
        switch (mode) {
            case QIcon::Disabled: return pal.color(QPalette::Disabled, QPalette::WindowText);
            case QIcon::Selected: return pal.color(QPalette::Active, QPalette::HighlightedText);
            default: return pal.color(QPalette::Active, QPalette::WindowText);
        }
    }

    QByteArray m_svg;
    size_t m_hash;
};

}  // namespace

bool isDark() { return QApplication::palette().color(QPalette::Window).lightness() < 128; }

QColor mix(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor laneColor(int index) {
    static const QColor light[] = {QColor(0x2f80ed), QColor(0x27ae60), QColor(0xe2711d), QColor(0x9b51e0),
                                   QColor(0xeb5757), QColor(0x00a3a3), QColor(0xc99a06), QColor(0xd94690)};
    static const QColor dark[] = {QColor(0x5a9cff), QColor(0x4cc77d), QColor(0xff9447), QColor(0xb47bff),
                                  QColor(0xff7a7a), QColor(0x2cc9c9), QColor(0xf0c040), QColor(0xff79b0)};
    const int i = ((index % 8) + 8) % 8;
    return isDark() ? dark[i] : light[i];
}

QColor statusColor(char status) {
    switch (status) {
        case 'A':
        case '?': return QColor(0x2da44e);
        case 'D': return QColor(0xd1242f);
        case 'R':
        case 'C': return QColor(0x6f42c1);
        case 'U': return QColor(0xe5534b);
        case 'T': return QColor(0x0969da);
        default: return QColor(0xd4a72c);  // modified
    }
}

QIcon statusIcon(char status) {
    static QHash<int, QIcon> cache;
    const int key = int(uchar(status)) | (isDark() ? 0x100 : 0);
    if (auto it = cache.constFind(key); it != cache.cend()) return *it;
    QIcon icon;
    for (int size : {16, 24, 32, 48}) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r(size * 0.06, size * 0.06, size * 0.88, size * 0.88);
        p.setPen(Qt::NoPen);
        p.setBrush(statusColor(status));
        p.drawRoundedRect(r, size * 0.22, size * 0.22);
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPixelSize(qMax(8, int(size * 0.64)));
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, QString(QLatin1Char(status)));
        p.end();
        icon.addPixmap(pm);
    }
    cache.insert(key, icon);
    return icon;
}

QString statusName(char status) {
    switch (status) {
        case 'M': return QObject::tr("Modified");
        case 'A': return QObject::tr("Added");
        case 'D': return QObject::tr("Deleted");
        case 'R': return QObject::tr("Renamed");
        case 'C': return QObject::tr("Copied");
        case 'T': return QObject::tr("Type changed");
        case 'U': return QObject::tr("Conflicted");
        case '?': return QObject::tr("Untracked");
        default: return QObject::tr("Changed");
    }
}

QColor badgeColor(Badge kind) {
    switch (kind) {
        case Badge::Head: return QColor(0xcf222e);
        case Badge::CurrentBranch: return QColor(0x1f883d);
        case Badge::Local: return QColor(0x0969da);
        case Badge::Remote: return QColor(0x8250df);
        case Badge::Tag: return QColor(0xbc4c00);
        case Badge::Stash: return QColor(0x6e7781);
    }
    return QColor(0x6e7781);
}

DiffColors diffColors() {
    const QPalette pal = QApplication::palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const bool dark = isDark();
    const QColor green(46, 160, 67);
    const QColor red(218, 54, 51);
    DiffColors c;
    c.addBg = mix(base, green, dark ? 0.20 : 0.15);
    c.delBg = mix(base, red, dark ? 0.20 : 0.13);
    c.addGutter = mix(base, green, dark ? 0.34 : 0.26);
    c.delGutter = mix(base, red, dark ? 0.34 : 0.24);
    c.hunkBg = mix(base, pal.color(QPalette::Highlight), dark ? 0.16 : 0.09);
    c.gutterBg = mix(base, pal.color(QPalette::Window), 0.65);
    c.gutterFg = mix(text, base, 0.5);
    c.markerBg = mix(base, QColor(155, 81, 224), dark ? 0.35 : 0.25);
    c.oursBg = mix(base, QColor(47, 128, 237), dark ? 0.20 : 0.12);
    c.theirsBg = mix(base, QColor(226, 113, 29), dark ? 0.20 : 0.12);
    return c;
}

QIcon actionIcon(const QString& name) {
    static QHash<QString, QByteArray> svgs;
    auto it = svgs.find(name);
    if (it == svgs.end()) {
        QFile f(QStringLiteral(":/icons/actions/%1.svg").arg(name));
        it = svgs.insert(name, f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray());
    }
    if (it->isEmpty()) return {};
    return QIcon(new TintedSvgEngine(*it, qHash(name)));
}

QIcon appIcon() {
    QIcon icon;
    for (int size : {16, 22, 24, 32, 48, 64, 128, 256}) {
        icon.addFile(QStringLiteral(":/icons/hicolor/%1x%1/apps/aslgc.png").arg(size), QSize(size, size));
    }
    return icon;
}

}  // namespace aslgc::theme
