#include "NativeRig.hpp"
#include <QFile>
#include <QPainter>
#include <QPainterPath>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace NativeRig {
namespace {
struct Reader {
    QByteArray data; qsizetype pos = 0;
    QByteArray take(qsizetype n) {
        if (n < 0 || n > data.size()-pos) throw std::runtime_error("Truncated PSD");
        auto result=data.mid(pos,n); pos+=n; return result;
    }
    quint32 num(int n) { auto b=take(n); quint32 v=0; for(auto c:b) v=(v<<8)|quint8(c); return v; }
    void skip(qsizetype n) { take(n); }
};
struct Channel { int id; quint32 length; };
struct Record {
    QString name; QRect rect, mask; int maskDefault=255, maskFlags=2, opacity=255;
    bool hidden=false; QVector<Channel> channels;
};
QRect readRect(Reader& r) {
    const int top=qint32(r.num(4)),left=qint32(r.num(4));
    const int bottom=qint32(r.num(4)),right=qint32(r.num(4));
    if(right<left||bottom<top||qint64(right)-left>8192||qint64(bottom)-top>8192)
        throw std::runtime_error("Invalid PSD rectangle");
    return QRect(left,top,right-left,bottom-top);
}
QByteArray decode(QByteArray data,int w,int h) {
    Reader r{data}; int compression=r.num(2);
    if(w<=0||h<=0) return {};
    if(qint64(w)*h>32000000) throw std::runtime_error("PSD channel too large");
    if(compression==0) return r.take(qsizetype(w)*h);
    if(compression!=1) throw std::runtime_error("PSD requires raw or PackBits channels");
    QVector<int> lengths(h); for(int& length:lengths) length=r.num(2);
    QByteArray result; result.reserve(w*h);
    for(int y=0;y<h;y++) {
        Reader row{r.take(lengths[y])}; QByteArray line;
        while(row.pos<row.data.size()) {
            int n=qint8(row.num(1));
            if(n>=0) line+=row.take(n+1);
            else if(n!=-128) line+=QByteArray(1-n,char(row.num(1)));
            if(line.size()>w) throw std::runtime_error("Invalid PackBits row");
        }
        if(line.size()!=w) throw std::runtime_error("Incomplete PackBits row");
        result+=line;
    }
    return result;
}
QRect alphaBounds(const QImage& image) {
    int x0=image.width(),y0=image.height(),x1=-1,y1=-1;
    for(int y=0;y<image.height();y++) {
        auto row=reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for(int x=0;x<image.width();x++) if(qAlpha(row[x])>8) {
            x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);
        }
    }
    return x1<x0?QRect():QRect(QPoint(x0,y0),QPoint(x1,y1));
}
void addLayer(Model& model,QString name,const QImage& full,int side=-1) {
    QRect bounds=alphaBounds(full); if(bounds.isEmpty()) return;
    Layer l; l.name=name;l.side=side;l.rect=bounds;l.image=full.copy(bounds);
    l.body=name=="topwear"||name=="handwear";
    l.hair=name.contains("hair");
    if(name=="back hair")l.depth=.7;
    else if(name=="front hair")l.depth=1.28;
    else if(name=="headwear")l.depth=1.2;
    else if(name=="nose")l.depth=1.15;
    else if(name=="topwear")l.depth=.82;
    else if(name=="handwear")l.depth=.86;
    else if(name=="ears")l.depth=.96;
    else if(name.startsWith("eye")||name=="irides")l.depth=1.08;
    model.layers.push_back(l);
}
double smooth(double x) {x=std::clamp(x,0.,1.);return x*x*(3-2*x);}
}

Model loadPsd(const QString& path) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot open avatar PSD");
    if(f.size()>128*1024*1024)throw std::runtime_error("PSD exceeds 128 MiB");
    Reader r{f.readAll()};
    if(r.take(4)!="8BPS"||r.num(2)!=1)throw std::runtime_error("Expected PSD v1");
    r.skip(6);const int channels=r.num(2),height=r.num(4),width=r.num(4);
    const int depth=r.num(2),mode=r.num(2);
    if(depth!=8||mode!=3||channels<3||width<=0||height<=0||width>4096||height>4096)
        throw std::runtime_error("Expected RGB/8-bit PSD up to 4096px");
    r.skip(r.num(4));r.skip(r.num(4));
    Reader section{r.take(r.num(4))};Reader layers{section.take(section.num(4))};
    int count=std::abs(qint16(layers.num(2)));
    if(count>128)throw std::runtime_error("Too many PSD layers");
    if(qint64(count)*width*height>128000000)throw std::runtime_error("PSD layer memory limit exceeded");
    QVector<Record> records;
    for(int i=0;i<count;i++) {
        Record rec;rec.rect=readRect(layers);int n=layers.num(2);
        if(n>16)throw std::runtime_error("Too many PSD channels");
        for(int c=0;c<n;c++)rec.channels.push_back({qint16(layers.num(2)),layers.num(4)});
        if(layers.take(4)!="8BIM")throw std::runtime_error("Invalid PSD blend signature");
        auto blend=layers.take(4);rec.opacity=layers.num(1);
        if(layers.num(1)!=0)throw std::runtime_error("PSD clipping layers are unsupported");
        rec.hidden=layers.num(1)&2;layers.skip(1);
        if(blend!="norm"&&blend!="pass")throw std::runtime_error("Only normal PSD layers are supported");
        Reader extra{layers.take(layers.num(4))};auto maskData=extra.take(extra.num(4));
        if(maskData.size()>=18){Reader mask{maskData};rec.mask=readRect(mask);rec.maskDefault=mask.num(1);rec.maskFlags=mask.num(1);}
        if(rec.maskFlags&1)rec.mask.translate(rec.rect.topLeft());
        extra.skip(extra.num(4));int len=extra.num(1);rec.name=QString::fromLatin1(extra.take(len));extra.skip((4-(len+1)%4)%4);
        while(extra.data.size()-extra.pos>=12) {
            auto sig=extra.take(4);auto key=extra.take(4);int size=extra.num(4);
            Reader block{extra.take(size)};if(size%2&&extra.pos<extra.data.size())extra.skip(1);
            if(sig=="8BIM"&&key=="luni") {
                int chars=block.num(4);if(chars<0||chars>4096)throw std::runtime_error("PSD layer name too long");
                QString name;for(int k=0;k<chars;k++)name+=QChar(block.num(2));rec.name=name;
            }
        }
        records.push_back(rec);
    }
    QVector<QPair<QString,QImage>> images;
    for(const auto& rec:records) {
        QMap<int,QByteArray> planes;
        for(const auto& c:rec.channels) {
            QSize size=c.id==-2?rec.mask.size():rec.rect.size();
            auto bytes=layers.take(c.length);
            if(c.id>=-2&&c.id<=2)planes[c.id]=decode(bytes,size.width(),size.height());
        }
        if(rec.hidden||rec.rect.isEmpty())continue;
        if(!planes.contains(0)||!planes.contains(1)||!planes.contains(2))continue;
        QImage full(width,height,QImage::Format_ARGB32);full.fill(Qt::transparent);
        for(int y=0;y<rec.rect.height();y++)for(int x=0;x<rec.rect.width();x++) {
            int gx=x+rec.rect.x(),gy=y+rec.rect.y(),p=y*rec.rect.width()+x;
            if(gx<0||gy<0||gx>=width||gy>=height)continue;
            auto sample=[&](int id,int fallback){auto it=planes.constFind(id);return it==planes.cend()?fallback:int(quint8(it.value()[p]));};
            int alpha=sample(-1,255)*rec.opacity/255;
            if(!(rec.maskFlags&2)&&planes.contains(-2)) {
                int mx=gx-rec.mask.x(),my=gy-rec.mask.y(),m=rec.maskDefault;
                if(mx>=0&&my>=0&&mx<rec.mask.width()&&my<rec.mask.height())m=quint8(planes[-2][my*rec.mask.width()+mx]);
                if(rec.maskFlags&4)m=255-m;
                alpha=alpha*m/255;
            }
            full.setPixel(gx,gy,qRgba(sample(0,0),sample(1,0),sample(2,0),alpha));
        }
        // Some PSD exporters include a trailing UTF-16 NUL in the Unicode name.
        auto name=rec.name;
        while(name.endsWith(QChar(0)))name.chop(1);
        images.push_back({name.trimmed().toLower(),full});
    }
    Model model;model.canvas={width,height};
    // Preserve the bottom-to-top records written by the supplied PSD exporter.
    for(const auto& pair:images)if(pair.first=="face")model.face=alphaBounds(pair.second);
    if(model.face.isEmpty())throw std::runtime_error("PSD face layer is missing");
    int split=qRound(model.face.center().x());
    for(const auto& pair:images) {
        QString name=pair.first=="mouth"?"mouth_close":pair.first;
        if(name=="eyewhite"||name=="irides"||name=="eyelash"||name=="eyebrow") {
            for(int side=0;side<2;side++) {
                QImage image=pair.second;
                QPainter p(&image);p.setCompositionMode(QPainter::CompositionMode_Source);
                p.fillRect(side==0?QRect(split,0,width-split,height):QRect(0,0,split,height),Qt::transparent);p.end();
                if(name=="eyewhite")model.eyes[side]=alphaBounds(image);
                addLayer(model,name,image,side);
            }
        } else {if(name=="mouth_close")model.mouth=alphaBounds(pair.second);addLayer(model,name,pair.second);}
    }
    if(model.mouth.isEmpty()||model.eyes[0].isEmpty()||model.eyes[1].isEmpty())throw std::runtime_error("PSD mouth/eyes are missing");
    model.neck={model.face.center().x(),model.face.bottom()+model.face.height()*.15};
    model.bodyPivot={width*.5,double(height)};
    // Supplement the missing expression drawings in native QPainter paths.
    for(int side=0;side<2;side++) {
        auto eye=model.eyes[side];QImage full(width,height,QImage::Format_ARGB32);full.fill(Qt::transparent);
        QPainter p(&full);p.setRenderHint(QPainter::Antialiasing);p.setPen(QPen(QColor("#35212b"),3.2,Qt::SolidLine,Qt::RoundCap));
        QPainterPath curve;curve.moveTo(eye.left(),eye.center().y());curve.quadTo(eye.center().x(),eye.top()-eye.height()*.2,eye.right(),eye.center().y());p.drawPath(curve);p.end();
        auto at=std::find_if(model.layers.begin(),model.layers.end(),[](const Layer& l){return l.name=="front hair";});
        int index=int(at-model.layers.begin());addLayer(model,"eye_close",full,side);auto l=model.layers.takeLast();model.layers.insert(index,l);
    }
    QImage full(width,height,QImage::Format_ARGB32);full.fill(Qt::transparent);
    QRectF mouth(model.mouth.center().x()-model.mouth.width()*.45,model.mouth.center().y()-model.mouth.width()*.38,model.mouth.width()*.9,model.mouth.width()*.76);
    QPainter p(&full);p.setRenderHint(QPainter::Antialiasing);QPainterPath shape;shape.addEllipse(mouth);
    p.fillPath(shape,QColor("#542237"));p.setPen(QPen(QColor("#a75565"),.8));p.drawPath(shape);p.setClipPath(shape);
    p.setBrush(QColor("#e28c9a"));p.setPen(Qt::NoPen);p.drawEllipse(QRectF(mouth.left()+mouth.width()*.08,mouth.bottom()-mouth.height()*.3,mouth.width()*.84,mouth.height()*.7));
    p.fillRect(QRectF(mouth.left()+3,mouth.top(),mouth.width()-6,3),QColor("#fff0e8"));p.end();
    int index=0;for(int i=0;i<model.layers.size();i++)if(model.layers[i].name=="mouth_close")index=i+1;
    addLayer(model,"mouth_open",full);auto l=model.layers.takeLast();model.layers.insert(index,l);
    return model;
}

// Head parallax, local lid compression and tip-weighted spring displacement
// reimplemented from Anime2.5DRig (MIT, Copyright 2026 hakoniwa).
QPointF deform(const Model& m,const Layer& l,QPointF p,double t,double mouth,double blink,double hair) {
    double x=p.x(),y=p.y(),scale=m.face.width()/330.;
    if(l.side>=0&&(l.name=="eyewhite"||l.name=="eyelash"||l.name=="irides")) {
        double cy=m.eyes[l.side].center().y();y=cy+(y-cy)*(1-.85*blink);
    }
    if(l.name=="mouth_open")y=m.mouth.center().y()+(y-m.mouth.center().y())*(.35+.65*mouth);
    if(l.name=="face"&&y>m.mouth.center().y())y+=mouth*6*scale*smooth((y-m.mouth.center().y())/std::max(1.,m.face.bottom()-m.mouth.center().y()));
    double ax=.13*std::sin(t*.42)+.05*std::sin(t*1.13),ay=.08*std::sin(t*.31+1.7),az=.07*std::sin(t*.23+.5)*.07;
    double hw=l.body?.16:1.;if(l.name=="neck")hw=.55;
    double rx=x-m.neck.x(),ry=y-m.neck.y();
    x+=hw*(rx*std::cos(az)-ry*std::sin(az)-rx+scale*(ax*(14+40*(l.depth-1))+ax*(m.neck.y()-y)*.028));
    y+=hw*(rx*std::sin(az)+ry*std::cos(az)-ry-scale*ay*(9+30*(l.depth-1)));
    y-=(l.body?2.:1.6)*scale*(.5+.5*std::sin(t*2*3.141592653589793/3.4));
    if(l.name=="topwear") {
        // Breathing expands the upper torso around the neck, fading toward
        // the waist so the lower edge remains planted.
        double breath=.5+.5*std::sin(t*2*3.141592653589793/3.4);
        double upper=smooth((m.bodyPivot.y()-p.y())/std::max(1.,m.bodyPivot.y()-m.neck.y()));
        x=m.neck.x()+(x-m.neck.x())*(1+breath*.003*upper);
        y-=breath*2.2*scale*upper;
    }
    if(l.hair){double u=std::clamp((p.y()-l.rect.top())/std::max(1.,l.rect.height()),0.,1.);x+=hair*std::pow(u,l.name=="front hair"?1.8:2.1);}
    // Apply the same waist-pivot rotation to every part after local motion.
    // This carries the head, neck, arms and hair with the torso rather than
    // separating the layers at their joints.
    double bodyAngle=.028*(.32*std::sin(t*.65)+.12*std::sin(t*1.17+.4));
    double bx=x-m.bodyPivot.x(),by=y-m.bodyPivot.y();
    return {m.bodyPivot.x()+bx*std::cos(bodyAngle)-by*std::sin(bodyAngle),
            m.bodyPivot.y()+bx*std::sin(bodyAngle)+by*std::cos(bodyAngle)};
}
double opacity(const Layer& l,double mouth,double blink) {
    if(l.name=="mouth_open")return smooth((mouth-.08)/.25);
    if(l.name=="mouth_close")return 1-smooth((mouth-.08)/.25);
    if(l.name=="eye_close")return smooth((blink-.4)/.4);
    if(l.name=="eyewhite"||l.name=="irides"||l.name=="eyelash")return 1-smooth((blink-.4)/.4);
    return 1;
}
}
