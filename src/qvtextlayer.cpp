#include "qvtextlayer.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPalette>
#include <QtMath>

QVTextLayer::QVTextLayer(QGraphicsItem *parent) : QGraphicsItem(parent)
{
    setAcceptedMouseButtons(Qt::NoButton); // the view drives interaction
    setFlag(QGraphicsItem::ItemHasNoContents, false);
}

void QVTextLayer::setOcrResult(const QByteArray &json, const QSize &size)
{
    prepareGeometryChange();
    glyphs.clear();
    lineRects.clear();
    lineStart.clear();
    selStart = selEnd = 0;
    imageSize = size;

    const QJsonObject root = QJsonDocument::fromJson(json).object();
    if (root.contains("error"))
        qWarning("qview-ocr: %s", qPrintable(root.value("error").toString()));

    const QJsonArray lines = root.value("lines").toArray();
    for (const auto &lineVal : lines) {
        const QJsonArray chars = lineVal.toObject().value("chars").toArray();
        if (chars.isEmpty())
            continue;
        const int lineIndex = lineRects.size();
        lineStart.append(glyphs.size());
        QRectF lineRect;
        for (const auto &cv : chars) {
            const QJsonObject c = cv.toObject();
            Glyph g;
            g.text = c.value("c").toString();
            g.rect = QRectF(c.value("x").toDouble(), c.value("y").toDouble(),
                            c.value("w").toDouble(), c.value("h").toDouble());
            g.line = lineIndex;
            lineRect = lineRect.isNull() ? g.rect : lineRect.united(g.rect);
            glyphs.append(g);
        }
        lineRects.append(lineRect);
    }
    lineStart.append(glyphs.size());
    update();
}

void QVTextLayer::clear()
{
    setOcrResult(QByteArray(), QSize());
}

int QVTextLayer::lineAt(const QPointF &pos, bool nearest) const
{
    int best = -1;
    qreal bestDist = 0;
    for (int i = 0; i < lineRects.size(); i++) {
        const QRectF &r = lineRects[i];
        // Small padding so the I-beam appears just around the text too.
        const qreal pad = r.height() * 0.25;
        if (r.adjusted(-pad, -pad, pad, pad).contains(pos))
            return i;
        if (!nearest)
            continue;
        // Vertical distance dominates so dragging sideways stays on a line.
        const qreal dy = pos.y() < r.top() ? r.top() - pos.y()
                : pos.y() > r.bottom()      ? pos.y() - r.bottom()
                                            : 0;
        const qreal dx = pos.x() < r.left() ? r.left() - pos.x()
                : pos.x() > r.right()       ? pos.x() - r.right()
                                            : 0;
        const qreal dist = dy * 4 + dx;
        if (best < 0 || dist < bestDist) {
            best = i;
            bestDist = dist;
        }
    }
    return best;
}

bool QVTextLayer::isOverText(const QPointF &pos) const
{
    return lineAt(pos, false) >= 0;
}

int QVTextLayer::caretAt(const QPointF &pos) const
{
    if (glyphs.isEmpty())
        return 0;
    const int line = lineAt(pos, true);
    const QRectF &lr = lineRects[line];
    int caret = lineStart[line];
    for (int i = lineStart[line]; i < lineStart[line + 1]; i++) {
        if (pos.x() > glyphs[i].rect.center().x())
            caret = i + 1;
    }
    // Dragging well above the first / below the last line snaps to the very
    // start / end, so "drag past the text" selects everything up to there.
    if (pos.y() < lr.top() - lr.height() && line == 0)
        caret = 0;
    if (pos.y() > lr.bottom() + lr.height() && line == lineRects.size() - 1)
        caret = glyphs.size();
    return caret;
}

int QVTextLayer::glyphAt(const QPointF &pos) const
{
    const int caret = caretAt(pos);
    const int line = lineAt(pos, true);
    if (line < 0)
        return -1;
    // Prefer the glyph actually under the cursor, else the one before the caret.
    for (int i = lineStart[line]; i < lineStart[line + 1]; i++) {
        if (pos.x() >= glyphs[i].rect.left() && pos.x() <= glyphs[i].rect.right())
            return i;
    }
    return qBound(lineStart[line], caret - 1, lineStart[line + 1] - 1);
}

void QVTextLayer::setSelection(int anchorCaret, int cursorCaret)
{
    const int s = qMin(anchorCaret, cursorCaret);
    const int e = qMax(anchorCaret, cursorCaret);
    if (s == selStart && e == selEnd)
        return;
    selStart = s;
    selEnd = e;
    update();
}

void QVTextLayer::selectWordAt(const QPointF &pos)
{
    const int g = glyphAt(pos);
    if (g < 0)
        return;
    const int line = glyphs[g].line;
    const bool space = glyphs[g].text.trimmed().isEmpty();
    int s = g, e = g + 1;
    while (s > lineStart[line] && glyphs[s - 1].text.trimmed().isEmpty() == space)
        s--;
    while (e < lineStart[line + 1] && glyphs[e].text.trimmed().isEmpty() == space)
        e++;
    setSelection(s, e);
}

void QVTextLayer::selectLineAt(const QPointF &pos)
{
    const int line = lineAt(pos, true);
    if (line >= 0)
        setSelection(lineStart[line], lineStart[line + 1]);
}

void QVTextLayer::clearSelection()
{
    setSelection(0, 0);
}

QString QVTextLayer::selectedText() const
{
    QString out;
    for (int i = selStart; i < selEnd; i++) {
        if (i > selStart && glyphs[i].line != glyphs[i - 1].line)
            out += QLatin1Char('\n');
        out += glyphs[i].text;
    }
    return out;
}

QRectF QVTextLayer::boundingRect() const
{
    return QRectF(QPointF(0, 0), QSizeF(imageSize));
}

void QVTextLayer::paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
                        QWidget *widget)
{
    Q_UNUSED(option)
    Q_UNUSED(widget)
    if (!hasSelection())
        return;

    QColor color = QApplication::palette().color(QPalette::Highlight);
    color.setAlpha(110);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->setCompositionMode(QPainter::CompositionMode_Multiply);

    // One band per line: from first to last selected glyph, full line height,
    // so highlights look like a text selection instead of per-char boxes.
    int i = selStart;
    while (i < selEnd) {
        const int line = glyphs[i].line;
        qreal left = glyphs[i].rect.left();
        qreal right = glyphs[i].rect.right();
        while (i < selEnd && glyphs[i].line == line) {
            left = qMin(left, glyphs[i].rect.left());
            right = qMax(right, glyphs[i].rect.right());
            i++;
        }
        const QRectF &lr = lineRects[line];
        const qreal pad = lr.height() * 0.1;
        painter->drawRect(QRectF(left, lr.top() - pad, right - left, lr.height() + 2 * pad));
    }
}
