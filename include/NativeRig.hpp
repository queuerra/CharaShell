#pragma once
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

namespace NativeRig {
struct Layer {
    QString name;
    QImage image;
    QRectF rect;
    double depth = 1;
    bool body = false, hair = false;
    int side = -1; // image left / image right
};
struct Model {
    QSize canvas;
    QVector<Layer> layers;
    QRectF face, eyes[2], mouth;
    QPointF neck, bodyPivot;
};
// RGB/8-bit PSD v1, flat normal-blended layers, raw/PackBits channels.
// Unsupported encodings fail explicitly instead of showing a damaged model.
Model loadPsd(const QString& path);
QPointF deform(const Model& model, const Layer& layer, QPointF point,
               double seconds, double mouth, double blink, double hairOffset);
double opacity(const Layer& layer, double mouth, double blink);
}
