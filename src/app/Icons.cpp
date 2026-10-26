#include "app/Icons.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>

#include <cmath>

namespace {

constexpr int kSize = 64; // drawn at 64 px, Qt scales down

void drawIcon(QPainter& p, const QString& n, const QColor& ink, const QColor& accent) {
  QPen pen(ink, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  auto dot = [&](double x, double y) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawEllipse(QPointF(x, y), 6, 6);
    p.restore();
  };
  auto box3d = [&](double top) { // simple isometric block, top face at `top`
    QPainterPath path;
    path.moveTo(12, top + 10);
    path.lineTo(32, top);
    path.lineTo(52, top + 10);
    path.lineTo(52, 50);
    path.lineTo(32, 60);
    path.lineTo(12, 50);
    path.closeSubpath();
    p.drawPath(path);
    p.drawLine(QPointF(12, top + 10), QPointF(32, top + 20));
    p.drawLine(QPointF(52, top + 10), QPointF(32, top + 20));
    p.drawLine(QPointF(32, top + 20), QPointF(32, 60));
  };

  if (n == "app") { // the caliper logo, same design as packaging/*.svg at half scale
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x25, 0x63, 0xa8));
    p.drawRoundedRect(QRectF(4, 4, 56, 56), 12, 12);
    const QColor light(0xf2, 0xf5, 0xf8);
    p.setBrush(light);
    p.drawRoundedRect(QRectF(10, 17, 46, 8), 1.5, 1.5);
    p.drawPolygon(QPolygonF({QPointF(10, 17), QPointF(19, 17), QPointF(19, 48), QPointF(14.5, 53), QPointF(10, 48)}));
    p.setBrush(QColor(0xff, 0xb0, 0x2e));
    p.drawPolygon(QPolygonF({QPointF(29, 14), QPointF(40, 14), QPointF(40, 28), QPointF(38, 28), QPointF(38, 48),
                             QPointF(33.5, 53), QPointF(29, 48)}));
    return;
  }
  if (n == "select") {
    QPainterPath arrow;
    arrow.moveTo(16, 8);
    arrow.lineTo(16, 50);
    arrow.lineTo(27, 40);
    arrow.lineTo(35, 56);
    arrow.lineTo(42, 52);
    arrow.lineTo(34, 37);
    arrow.lineTo(48, 36);
    arrow.closeSubpath();
    p.drawPath(arrow);
  } else if (n == "line") {
    p.drawLine(12, 52, 52, 12);
    dot(12, 52);
    dot(52, 12);
  } else if (n == "circle") {
    p.drawEllipse(QPointF(32, 32), 22, 22);
    dot(32, 32);
  } else if (n == "arc") {
    p.drawArc(QRectF(10, 10, 44, 44), 0, 90 * 16);
    p.drawArc(QRectF(10, 10, 44, 44), 90 * 16, 60 * 16);
    dot(54, 32);
    dot(32 - 22 * 0.5, 32 - 22 * 0.866);
    dot(32, 32);
  } else if (n == "sketch") {
    p.drawRect(10, 14, 44, 36);
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(18, 42, 46, 22);
  } else if (n == "face-sketch") {
    box3d(16);
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(22, 22, 42, 22);
  } else if (n == "finish") {
    p.setPen(QPen(accent, 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF({QPointF(12, 34), QPointF(26, 48), QPointF(52, 18)}));
  } else if (n == "extrude") {
    p.drawRect(12, 40, 40, 16);
    p.setPen(QPen(accent, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(32, 36, 32, 8);
    p.drawPolyline(QPolygonF({QPointF(22, 18), QPointF(32, 8), QPointF(42, 18)}));
  } else if (n == "revolve") {
    p.drawEllipse(QRectF(10, 20, 44, 24));
    p.setPen(QPen(accent, 4, Qt::DashLine, Qt::RoundCap));
    p.drawLine(32, 6, 32, 58);
  } else if (n == "fillet") {
    QPainterPath path;
    path.moveTo(12, 12);
    path.lineTo(12, 52);
    path.lineTo(52, 52);
    p.drawPath(path);
    p.setPen(QPen(accent, 5, Qt::SolidLine, Qt::RoundCap));
    QPainterPath round;
    round.moveTo(52, 12);
    round.lineTo(36, 12);
    round.quadTo(12, 12, 12, 36);
    p.drawPath(round);
  } else if (n == "chamfer") {
    QPainterPath path;
    path.moveTo(12, 12);
    path.lineTo(12, 52);
    path.lineTo(52, 52);
    p.drawPath(path);
    p.setPen(QPen(accent, 5, Qt::SolidLine, Qt::RoundCap));
    p.drawPolyline(QPolygonF({QPointF(52, 12), QPointF(30, 12), QPointF(12, 30)}));
  } else if (n == "horizontal") {
    p.drawLine(10, 32, 54, 32);
  } else if (n == "vertical") {
    p.drawLine(32, 10, 32, 54);
  } else if (n == "parallel") {
    p.drawLine(14, 50, 34, 14);
    p.drawLine(30, 50, 50, 14);
  } else if (n == "perpendicular") {
    p.drawLine(32, 12, 32, 52);
    p.drawLine(12, 52, 52, 52);
  } else if (n == "tangent") {
    p.drawEllipse(QPointF(32, 38), 16, 16);
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(8, 22, 56, 22);
  } else if (n == "equal") {
    p.drawLine(14, 24, 50, 24);
    p.drawLine(14, 40, 50, 40);
  } else if (n == "coincident") {
    p.drawLine(10, 50, 32, 32);
    p.drawLine(32, 32, 56, 44);
    dot(32, 32);
  } else if (n == "dimension") {
    p.drawLine(10, 16, 10, 48);
    p.drawLine(54, 16, 54, 48);
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(14, 32, 50, 32);
    p.drawPolyline(QPolygonF({QPointF(22, 24), QPointF(14, 32), QPointF(22, 40)}));
    p.drawPolyline(QPolygonF({QPointF(42, 24), QPointF(50, 32), QPointF(42, 40)}));
  } else if (n == "fix") {
    p.drawRect(16, 30, 32, 24);
    p.drawArc(QRectF(22, 12, 20, 32), 0, 180 * 16);
  } else if (n == "fit") {
    p.drawPolyline(QPolygonF({QPointF(10, 24), QPointF(10, 10), QPointF(24, 10)}));
    p.drawPolyline(QPolygonF({QPointF(40, 10), QPointF(54, 10), QPointF(54, 24)}));
    p.drawPolyline(QPolygonF({QPointF(54, 40), QPointF(54, 54), QPointF(40, 54)}));
    p.drawPolyline(QPolygonF({QPointF(24, 54), QPointF(10, 54), QPointF(10, 40)}));
    p.drawRect(22, 22, 20, 20);
  } else if (n == "iso") {
    box3d(8);
  } else if (n == "rectangle") {
    p.drawRect(QRectF(10, 16, 44, 32));
    dot(10, 16);
    dot(54, 48);
  } else if (n == "polygon") {
    QPolygonF hex;
    for (int i = 0; i < 6; ++i) hex << QPointF(32 + 22 * std::cos(i * 3.14159265 / 3), 32 + 22 * std::sin(i * 3.14159265 / 3));
    p.drawPolygon(hex);
    dot(32, 32);
  } else if (n == "slot") {
    QPainterPath path;
    path.moveTo(20, 20);
    path.lineTo(44, 20);
    path.arcTo(QRectF(32, 20, 24, 24), 90, -180);
    path.lineTo(20, 44);
    path.arcTo(QRectF(8, 20, 24, 24), 270, -180);
    p.drawPath(path);
    dot(20, 32);
    dot(44, 32);
  } else if (n == "construction") {
    p.setPen(QPen(ink, 4, Qt::DashLine, Qt::RoundCap));
    p.drawLine(10, 50, 54, 14);
    p.drawEllipse(QPointF(32, 32), 14, 14);
  } else if (n == "shell") {
    box3d(16);
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath inner;
    inner.moveTo(20, 27);
    inner.lineTo(32, 21);
    inner.lineTo(44, 27);
    inner.lineTo(32, 33);
    inner.closeSubpath();
    p.drawPath(inner);
  } else if (n == "pattern") {
    for (int i = 0; i < 3; ++i) p.drawRect(QRectF(8 + i * 17, 22, 12, 20));
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(10, 52, 54, 52);
  } else if (n == "measure") {
    p.drawLine(10, 50, 54, 14);
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap));
    for (int i = 1; i < 5; ++i) {
      const double x = 10 + i * 8.8, y = 50 - i * 7.2;
      p.drawLine(QPointF(x, y), QPointF(x + 5, y + 6));
    }
  } else if (n == "properties") {
    p.drawRect(QRectF(14, 22, 36, 30));
    p.drawArc(QRectF(24, 10, 16, 16), 0, 180 * 16);
    p.setPen(QPen(accent, 4));
    p.drawLine(22, 38, 42, 38);
  } else if (n == "error") {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(210, 50, 40));
    p.drawEllipse(QPointF(32, 32), 26, 26);
    p.setPen(QPen(Qt::white, 7, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(32, 18, 32, 36);
    p.drawPoint(32, 47);
  } else if (n == "trim") { // two crossing lines, one arm cut away
    p.drawLine(32, 8, 32, 56);
    p.drawLine(8, 32, 32, 32);
    p.setPen(QPen(accent, 4, Qt::DashLine, Qt::RoundCap));
    p.drawLine(36, 32, 56, 32);
  } else if (n == "sketch-fillet") {
    QPainterPath path;
    path.moveTo(10, 54);
    path.lineTo(10, 34);
    path.quadTo(10, 10, 34, 10);
    path.lineTo(54, 10);
    p.drawPath(path);
    dot(10, 34);
    dot(34, 10);
  } else if (n == "offset") {
    p.drawRoundedRect(QRectF(20, 20, 24, 24), 2, 2);
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawRoundedRect(QRectF(8, 8, 48, 48), 4, 4);
  } else if (n == "mirror") {
    p.drawPolygon(QPolygonF({QPointF(26, 14), QPointF(26, 50), QPointF(8, 50)}));
    p.setPen(QPen(accent, 4, Qt::DashLine, Qt::RoundCap));
    p.drawLine(32, 6, 32, 58);
    p.setPen(QPen(ink, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolygon(QPolygonF({QPointF(38, 14), QPointF(38, 50), QPointF(56, 50)}));
  } else if (n == "symmetric") {
    p.setPen(QPen(accent, 4, Qt::DashLine, Qt::RoundCap));
    p.drawLine(32, 6, 32, 58);
    dot(14, 32);
    dot(50, 32);
    p.drawLine(18, 32, 26, 32);
    p.drawLine(38, 32, 46, 32);
  } else if (n == "hole") {
    box3d(16);
    p.setPen(QPen(accent, 4));
    p.drawEllipse(QPointF(32, 26), 9, 5);
    p.drawLine(23, 26, 23, 44);
    p.drawLine(41, 26, 41, 44);
  } else if (n == "parameters") {
    QFont f = p.font();
    f.setPixelSize(30);
    f.setBold(true);
    f.setItalic(true);
    p.setFont(f);
    p.drawText(QRectF(4, 4, 56, 56), Qt::AlignCenter, "f(x)");
    p.setPen(QPen(accent, 4, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(10, 54, 54, 54);
  } else if (n == "import") {
    box3d(16);
    p.setPen(QPen(accent, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(32, 4, 32, 26);
    p.drawPolyline(QPolygonF({QPointF(24, 18), QPointF(32, 26), QPointF(40, 18)}));
  }
}

} // namespace

QIcon makeIcon(const QString& name) {
  const QPalette pal = QApplication::palette();
  QIcon icon;
  for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
    QColor ink = pal.color(QPalette::WindowText), accent(30, 120, 220);
    if (mode == QIcon::Disabled) {
      ink = pal.color(QPalette::Disabled, QPalette::WindowText);
      accent = ink;
    }
    QPixmap pix(kSize, kSize);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    drawIcon(p, name, ink, accent);
    p.end();
    icon.addPixmap(pix, mode);
  }
  return icon;
}
