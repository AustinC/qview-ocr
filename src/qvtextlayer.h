#ifndef QVTEXTLAYER_H
#define QVTEXTLAYER_H

#include <QGraphicsItem>
#include <QImage>
#include <QVector>

// Invisible overlay holding OCR'd text in image-pixel coordinates.
// Paints the selection highlight and maps points to caret positions, giving
// Preview-style "select text in an image" behaviour.
class QVTextLayer : public QGraphicsItem
{
public:
    struct Glyph
    {
        QString text;
        QRectF rect; // image pixels
        int line;
    };

    explicit QVTextLayer(QGraphicsItem *parent = nullptr);

    // Parses the JSON produced by the Rust OCR library.
    void setOcrResult(const QByteArray &json, const QSize &imageSize);
    void clear();

    bool hasText() const { return !glyphs.isEmpty(); }
    QSize sourceSize() const { return imageSize; }

    // True if pos (item coords) is on a recognised line of text.
    bool isOverText(const QPointF &pos) const;
    // Caret index (0..glyphs.size()) closest to pos; between glyph i-1 and i.
    int caretAt(const QPointF &pos) const;

    void setSelection(int anchorCaret, int cursorCaret);
    void selectWordAt(const QPointF &pos);
    void selectLineAt(const QPointF &pos);
    void clearSelection();
    bool hasSelection() const { return selStart != selEnd; }
    QString selectedText() const;

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget = nullptr) override;

private:
    int lineAt(const QPointF &pos, bool nearest) const;
    int glyphAt(const QPointF &pos) const;

    QVector<Glyph> glyphs;     // reading order
    QVector<QRectF> lineRects; // union of glyph rects per line
    QVector<int> lineStart;    // first glyph index of each line (+ sentinel)
    QSize imageSize;
    int selStart = 0;
    int selEnd = 0;
};

#endif // QVTEXTLAYER_H
