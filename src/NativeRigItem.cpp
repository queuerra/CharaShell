#include "NativeRigItem.hpp"
#include "AssetPath.hpp"
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGOpacityNode>
#include <QSGTextureMaterial>
#include <QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
struct PartNode {QSGOpacityNode* opacity;QSGGeometryNode* mesh;int nx,ny;};
struct RigNode : QSGNode {
    QVector<PartNode> parts;
    QVector<QSGTexture*> textures;
    ~RigNode() override {while(firstChild())delete firstChild();qDeleteAll(textures);}
};
}
NativeRigItem::NativeRigItem(QQuickItem* parent):QQuickItem(parent) {
    setFlag(ItemHasContents,true);
    connect(&loadWatcher_,&QFutureWatcher<LoadResult>::finished,this,[this] {
        auto result=loadWatcher_.result();
        loadRunning_=false;
        // A path change supersedes the running import. Only the latest request
        // may update the item, and at most one import per item runs at a time.
        if(runningLoad_!=requestedLoad_){startModelLoad();return;}
        model_=std::move(result.model);error_=std::move(result.error);loading_=false;
        modelChanged_=true;emit statusChanged();update();
    });
    loadModel();
    clock_.start();timer_.setInterval(16);
    connect(&timer_,&QTimer::timeout,this,[this] {
        double now=clock_.elapsed()/1000.,dt=std::clamp(now-last_,0.,.05);last_=now;time_=now;
        mouth_+=(targetMouth_-mouth_)*(1-std::exp(-14*dt));
        double phase=std::fmod(time_,4.2);
        blink_=phase<3.6?0:phase<3.7?(phase-3.6)/.1:phase<3.82?1:phase<4.02?1-(phase-3.82)/.2:0;
        if(blinkOverride_>=0)blink_=blinkOverride_;
        // Damped spring follows a slow head/wind driver; roots remain fixed.
        double target=5*std::sin(time_*.8)+2*std::sin(time_*1.9);
        velocity_+=(28*(target-spring_)-7*velocity_)*dt;spring_+=velocity_*dt;
        emit frameChanged();update();
    });
    timer_.start();
}
void NativeRigItem::setPsdPath(const QString& path) {
    const auto normalized=QDir::fromNativeSeparators(path.trimmed());
    if(psdPath_==normalized)return;
    psdPath_=normalized;
    loadModel();
    emit psdPathChanged();
}
void NativeRigItem::loadModel() {
    ++requestedLoad_;
    model_={};error_.clear();loading_=true;modelChanged_=true;
    emit statusChanged();update();
    // Let QML apply psdPath before starting, rather than importing the bundled
    // avatar in the constructor and then importing the configured file again.
    if(loadRunning_||loadQueued_)return;
    loadQueued_=true;
    QTimer::singleShot(0,this,[this]{loadQueued_=false;startModelLoad();});
}
void NativeRigItem::startModelLoad() {
    const auto path=psdPath_.isEmpty()?QStringLiteral(":/native-avatar/avatar.psd"):resolveAssetPath(psdPath_);
    loadRunning_=true;runningLoad_=requestedLoad_;
    // The worker owns only its path/result. Destroying the QML item neither
    // blocks the GUI nor leaves a worker accessing a deleted QQuickItem.
    loadWatcher_.setFuture(QtConcurrent::run([path] {
        LoadResult result;
        try {result.model=NativeRig::loadPsd(path);}
        catch(const std::exception& e) {
            result.error=QStringLiteral("アバターPSDを読み込めません。\n%1\n%2").arg(path,QString::fromUtf8(e.what()));
        }
        return result;
    }));
}
void NativeRigItem::setMouthOpen(qreal value) {
    value=std::isfinite(value)?std::clamp(value,qreal(0),qreal(1)):0;
    if(value==targetMouth_)return;
    targetMouth_=value;emit mouthOpenChanged();update();
}
void NativeRigItem::setBlinkAmount(qreal value) {
    value=std::isfinite(value)?std::clamp(value,qreal(-1),qreal(1)):-1;
    if(value==blinkOverride_)return;
    blinkOverride_=value;emit blinkAmountChanged();update();
}
QSGNode* NativeRigItem::updatePaintNode(QSGNode* previous,UpdatePaintNodeData*) {
    // The scene graph owns textures; rebuild them on its render thread after a switch.
    if(modelChanged_){delete previous;previous=nullptr;modelChanged_=false;}
    if(!ready()||width()<=0||height()<=0){delete previous;return nullptr;}
    auto* root=static_cast<RigNode*>(previous);
    if(!root) {
        root=new RigNode;
        for(const auto& layer:model_.layers) {
            auto* alpha=new QSGOpacityNode;
            auto* mesh=new QSGGeometryNode;
            int nx=std::max(1,int(std::ceil(layer.rect.width()/40.))),ny=std::max(1,int(std::ceil(layer.rect.height()/40.)));
            auto* geometry=new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(),nx*ny*6);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
            mesh->setGeometry(geometry);mesh->setFlag(QSGNode::OwnsGeometry);
            auto* material=new QSGTextureMaterial;
            auto* texture=window()->createTextureFromImage(layer.image.convertToFormat(QImage::Format_ARGB32_Premultiplied));
            texture->setFiltering(QSGTexture::Linear);material->setTexture(texture);material->setFiltering(QSGTexture::Linear);
            material->setFlag(QSGMaterial::Blending,true);
            mesh->setMaterial(material);mesh->setFlag(QSGNode::OwnsMaterial);
            alpha->appendChildNode(mesh);root->appendChildNode(alpha);
            root->parts.push_back({alpha,mesh,nx,ny});root->textures.push_back(texture);
        }
    }
    double scale=std::min(width()/model_.canvas.width(),height()/model_.canvas.height());
    QPointF origin((width()-model_.canvas.width()*scale)/2,(height()-model_.canvas.height()*scale)/2);
    float renderedBottom=-std::numeric_limits<float>::infinity();
    for(int i=0;i<model_.layers.size();i++) {
        const auto& layer=model_.layers[i];auto& part=root->parts[i];
        const auto opacity=NativeRig::opacity(layer,mouth_,blink_);
        part.opacity->setOpacity(opacity);
        auto* vertices=part.mesh->geometry()->vertexDataAsTexturedPoint2D();int k=0;
        auto vertex=[&](int x,int y) {
            double u=double(x)/part.nx,v=double(y)/part.ny;
            auto p=NativeRig::deform(model_,layer,{layer.rect.x()+u*layer.rect.width(),layer.rect.y()+v*layer.rect.height()},time_,mouth_,blink_,spring_);
            p=origin+p*scale;
            const auto yPosition=float(p.y());
            vertices[k++].set(float(p.x()),yPosition,float(u),float(v));
            if(opacity>0)renderedBottom=std::max(renderedBottom,yPosition);
        };
        for(int y=0;y<part.ny;y++)for(int x=0;x<part.nx;x++) {
            vertex(x,y);vertex(x+1,y);vertex(x,y+1);
            vertex(x+1,y);vertex(x+1,y+1);vertex(x,y+1);
        }
    }
    // Align the animated character, rather than the PSD's transparent canvas,
    // with the scene's bottom edge (the top of the lower cinema band).
    const float groundOffset=std::isfinite(renderedBottom)?float(height())-renderedBottom:0;
    for(auto& part:root->parts) {
        auto* geometry=part.mesh->geometry();
        auto* vertices=geometry->vertexDataAsTexturedPoint2D();
        for(int i=0;i<geometry->vertexCount();++i)vertices[i].y+=groundOffset;
        part.mesh->markDirty(QSGNode::DirtyGeometry);
    }
    return root;
}
