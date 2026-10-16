#include "ui/psdpage.h"
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QPainter>
#include <QPainterPath>
#include <QSignalBlocker>
#include <cmath>

namespace mca {
class PsdPlot : public QWidget {
public:
    std::shared_ptr<const PsdSnapshot> snapshot;
    QImage density;
    bool adaptive[4]={false,false,false,false};
    QRect occupied{0,0,1024,512};
    quint64 densityMax=1;
    explicit PsdPlot(QWidget *parent=nullptr):QWidget(parent){setMinimumSize(740,460);}
    void setSnapshot(std::shared_ptr<const PsdSnapshot> s) {
        snapshot=std::move(s);density=snapshot->densityImage;
        int x0=1024,x1=-1,y0=512,y1=-1;densityMax=1;
        for(int y=0;y<512;++y)for(int x=0;x<1024;++x){auto c=snapshot->density[y*1024+x];
            densityMax=qMax(densityMax,c);if(c){x0=qMin(x0,x);x1=qMax(x1,x);y0=qMin(y0,y);y1=qMax(y1,y);}}
        occupied=QRect(0,0,1024,512);
        if(x1>=x0){int px=qMax(2,(x1-x0)/20),py=qMax(2,(y1-y0)/20);
            occupied=QRect(QPoint(qMax(0,x0-px),qMax(0,y0-py)),QPoint(qMin(1023,x1+px),qMin(511,y1+py)));}
        update();
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);p.fillRect(rect(),Qt::white);p.setRenderHint(QPainter::Antialiasing);
        if(!snapshot){p.setPen(Qt::darkGray);p.drawText(rect(),Qt::AlignCenter,QStringLiteral("等待 PSD 事件"));return;}
        const auto &s=*snapshot;
        auto fmt=[](double v){return QString::number(std::abs(v)<1e-12?0:v,'g',3);};
        auto axes=[&](int id,QString title,QString xl,QString yl,double x0,double x1,double y0,double y1){
            double cw=width()/2.0,ch=height()/2.0;
            QRectF r(id%2*cw+76,id/2*ch+36,cw-(id==0?164:116),ch-102);
            p.setPen(QPen(QColor("#657080"),.8));p.drawRect(r);
            int exponent=std::abs(x1-x0)>=10000?int(std::floor(std::log10(qMax(std::abs(x0),std::abs(x1))))):0;
            double scale=std::pow(10.0,exponent);
            if(exponent)xl=QStringLiteral("Qtotal (10%1 ADC·sample)").arg(QString(QChar(QStringLiteral("⁰¹²³⁴⁵⁶⁷⁸⁹").at(exponent))));
            int nx=r.width()<300?3:4;
            for(int i=0;i<=nx;++i){double f=double(i)/nx,x=r.left()+f*r.width();
                p.drawLine(QPointF(x,r.bottom()),QPointF(x,r.bottom()-5));
                p.drawText(QRectF(x-34,r.bottom()+5,68,20),Qt::AlignHCenter,fmt((x0+f*(x1-x0))/scale));}
            for(int i=0;i<=4;++i){double f=i/4.0,y=r.bottom()-f*r.height();
                p.drawLine(QPointF(r.left(),y),QPointF(r.left()+5,y));
                p.drawText(QRectF(r.left()-57,y-10,51,20),Qt::AlignRight|Qt::AlignVCenter,fmt(y0+f*(y1-y0)));}
            p.setPen(QColor("#202833"));p.drawText(QRectF(r.left(),r.top()-27,r.width()+40,22),title);
            p.drawText(QRectF(r.left(),r.bottom()+28,r.width(),22),Qt::AlignCenter,xl);
            p.save();p.translate(r.left()-65,r.center().y());p.rotate(-90);
            p.drawText(QRectF(-r.height()/2,-10,r.height(),20),Qt::AlignCenter,yl);p.restore();return r;
        };
        double qs=(s.settings.xMax-s.settings.xMin)/1024,rs=(s.settings.rMax-s.settings.rMin)/512;
        QRect bins=adaptive[0]?occupied:QRect(0,0,1024,512);
        double rlo=s.settings.rMin+bins.y()*rs,rhi=rlo+bins.height()*rs;
        QRectF d=axes(0,QStringLiteral("(a) 总电荷–PSD"),QStringLiteral("Qtotal (ADC·sample)"),QStringLiteral("R = Qtail/Qtotal"),s.settings.xMin+bins.x()*qs,s.settings.xMin+(bins.x()+bins.width())*qs,rlo,rhi);
        p.drawImage(d,density,QRectF(bins.x(),512-bins.y()-bins.height(),bins.width(),bins.height()));
        if(s.settings.threshold>=rlo&&s.settings.threshold<=rhi){double y=d.bottom()-(s.settings.threshold-rlo)/(rhi-rlo)*d.height();
            p.setPen(QPen(QColor("#444444"),1,Qt::DashLine));p.drawLine(QPointF(d.left(),y),QPointF(d.right(),y));
            p.drawText(QRectF(d.left()+4,y-20,d.width()-8,18),Qt::AlignRight,QStringLiteral("R = %1").arg(fmt(s.settings.threshold)));}
        QRectF bar(d.right()+12,d.top(),12,d.height());
        for(int i=0;i<256;++i){double c=i/255.0;p.fillRect(QRectF(bar.left(),bar.bottom()-(i+1)*bar.height()/256,bar.width(),bar.height()/256+1),QColor::fromRgbF(1-.88*c,1-.60*c,1-.30*c));}
        p.setPen(QColor("#202833"));p.drawRect(bar);p.drawText(QRectF(bar.left()-5,bar.top()-24,80,20),QStringLiteral("Counts"));
        for(int i=0;i<=4;++i){double f=i/4.0;p.drawText(QRectF(bar.right()+4,bar.bottom()-f*bar.height()-10,64,20),Qt::AlignVCenter,fmt(std::expm1(f*std::log1p(double(densityMax)))));}
        p.drawText(QRectF(bar.left()-5,bar.bottom()+28,80,20),QStringLiteral("log(1+N)"));
        auto hist=[&](int id,const QVector<quint64> &v,QColor color,QString title,bool ratio){
            int lo=0,hi=int(v.size());
            if(adaptive[id]){while(lo<hi&&!v[lo])++lo;while(hi>lo&&!v[hi-1])--hi;
                if(lo==hi){lo=0;hi=int(v.size());}else{int pad=qMax(2,(hi-lo)/20);lo=qMax(0,lo-pad);hi=qMin(int(v.size()),hi+pad);}}
            double step=ratio?rs:qs,start=ratio?s.settings.rMin:s.settings.xMin,max=1;
            for(int i=lo;i<hi;++i)max=qMax(max,double(v[i]));
            if(id==1&&s.fit.valid)for(auto pt:s.fit.curve)if(pt.x()+.5>=lo&&pt.x()+.5<=hi)max=qMax(max,pt.y());
            double top=4*std::ceil(max*1.12/4);
            if(id==1)title=s.fit.valid?QStringLiteral("(b) ROI 投影（蓝：数据；红：拟合）"):QStringLiteral("(b) ROI 比值投影");
            QRectF r=axes(id,title,ratio?QStringLiteral("R = Qtail/Qtotal"):QStringLiteral("Qtotal (ADC·sample)"),QStringLiteral("Counts"),start+lo*step,start+hi*step,0,top);
            auto pt=[&](double x,double y){return QPointF(r.left()+(x-lo)*r.width()/(hi-lo),r.bottom()-y*r.height()/top);};
            QPainterPath path;path.moveTo(pt(lo,0));for(int i=lo;i<hi;++i){path.lineTo(pt(i,v[i]));path.lineTo(pt(i+1,v[i]));}path.lineTo(pt(hi,0));
            p.save();p.setClipRect(r);p.setPen(QPen(color,1.1));p.drawPath(path);
            if(id==1&&s.fit.valid&&!s.fit.curve.isEmpty()){QPainterPath fit;bool first=true;
                for(auto q:s.fit.curve){if(first){fit.moveTo(pt(q.x()+.5,q.y()));first=false;}else fit.lineTo(pt(q.x()+.5,q.y()));}
                p.setPen(QPen(QColor("#d62728"),1.5));p.drawPath(fit);}p.restore();

        };
        hist(1,s.projection,QColor("#3b566e"),QStringLiteral("(b) ROI 比值投影"),true);
        hist(2,s.neutron,QColor("#0072b2"),QStringLiteral("(c) 中子候选总电荷谱"),false);
        hist(3,s.gamma,QColor("#c66a00"),QStringLiteral("(d) γ候选总电荷谱"),false);
    }
};
PsdPage::PsdPage(QWidget *parent):QWidget(parent),m_session(this) {
    setObjectName("psdPage");auto layout=new QVBoxLayout(this);auto grid=new QGridLayout;
    auto spin=[&](QString label,double value,double min,double max,int row,int col,QString suffix={}){
        auto box=new QDoubleSpinBox;box->setRange(min,max);box->setDecimals(3);box->setValue(value);box->setSuffix(suffix);
        grid->addWidget(new QLabel(label),row,col*2);grid->addWidget(box,row,col*2+1);return box;
    };
    m_total=spin(QStringLiteral("总窗口"),1.2,.048,10,0,0,QStringLiteral(" μs"));
    m_pre=spin(QStringLiteral("前置"),0,0,9.992,0,1,QStringLiteral(" μs"));
    m_start=spin(QStringLiteral("尾部起点 / T"),.056,-10,10,0,2,QStringLiteral(" μs"));
    m_end=spin(QStringLiteral("尾部终点 / T"),1.2,-10,10,0,3,QStringLiteral(" μs"));
    for(auto b:{m_total,m_pre,m_start,m_end}){b->setSingleStep(.008);connect(b,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this,b]{
        QSignalBlocker block(b);b->setValue(qRound(b->value()/.008)*.008);
        auto c=channelConfig({});m_gates->setText(QStringLiteral("实际总窗口 [%1,%2) ns / %3 点；尾部门 [%4,%5) ns。阈值、极性、基线和滤波使用左侧共享设置。")
            .arg(-8*int(c.integrationPreSamples)).arg(8*(int(c.integrationPostSamples)-c.integrationPreSamples)).arg(c.integrationPostSamples).arg(8*c.psdTailStart).arg(8*c.psdTailEnd));});}
    m_xmin=spin(QStringLiteral("Q 显示下限"),0,-33554432,33554431,1,0);
    m_xmax=spin(QStringLiteral("Q 显示上限"),262144,-33554432,33554431,1,1);
    m_rmin=spin(QStringLiteral("R 显示下限"),0,-100,100,1,2);
    m_rmax=spin(QStringLiteral("R 显示上限"),1,-100,100,1,3);
    m_roiMin=spin(QStringLiteral("Q ROI 下限"),0,-33554432,33554431,2,0);
    m_roiMax=spin(QStringLiteral("Q ROI 上限"),262144,-33554432,33554431,2,1);
    m_fitMin=spin(QStringLiteral("拟合 R 下限"),0,-100,100,2,2);
    m_fitMax=spin(QStringLiteral("拟合 R 上限"),1,-100,100,2,3);
    m_threshold=spin(QStringLiteral("分类 R 阈值"),.2,-100,100,3,0);
    m_direction=new QCheckBox(QStringLiteral("高比值为中子候选"));m_direction->setChecked(true);grid->addWidget(m_direction,3,2,1,2);
    m_fit=new QCheckBox(QStringLiteral("双高斯 FOM（至少 100 条）"));grid->addWidget(m_fit,3,4,1,2);
    m_record=new QPushButton(QStringLiteral("设置原始事件记录文件"));grid->addWidget(m_record,4,0,1,3);
    m_open=new QPushButton(QStringLiteral("载入 PSD 文件"));grid->addWidget(m_open,4,3);
    auto exportResult=new QPushButton(QStringLiteral("导出统计与 FOM"));grid->addWidget(exportResult,4,4,1,2);
    auto clearButton=new QPushButton(QStringLiteral("清空统计"));grid->addWidget(clearButton,4,6,1,2);
    layout->addLayout(grid);m_gates=new QLabel(QStringLiteral("125 MS/s；总窗口含前置，最长 10 μs；8 ns 步长，窗口左闭右开。共享设置在左侧。"));m_gates->setWordWrap(true);layout->addWidget(m_gates);
    m_plot=new PsdPlot;
    auto options=new QHBoxLayout;options->addWidget(new QLabel(QStringLiteral("自适应范围：")));
    const QStringList names={QStringLiteral("总电荷–PSD"),QStringLiteral("ROI 比值投影"),QStringLiteral("中子候选谱"),QStringLiteral("γ候选谱")};
    for(int i=0;i<4;++i){auto box=new QCheckBox(names[i]);box->setObjectName(QStringLiteral("psdAutoRange%1").arg(i));
        box->setToolTip(QStringLiteral("按当前统计范围内的非零数据缩放，留约 5% 边距。不改变分箱、ROI、拟合或导出；取消后恢复设置范围。计数轴始终自动缩放。"));
        options->addWidget(box);connect(box,&QCheckBox::toggled,this,[this,i](bool on){m_plot->adaptive[i]=on;m_plot->update();});}
    options->addStretch();layout->addLayout(options);layout->addWidget(m_plot,1);m_status=new QLabel;m_status->setWordWrap(true);layout->addWidget(m_status);
    m_transport=new QLabel;layout->addWidget(m_transport);
    for(auto b:{m_xmin,m_xmax,m_rmin,m_rmax,m_roiMin,m_roiMax,m_fitMin,m_fitMax,m_threshold})
        connect(b,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{updateSettings();});
    connect(m_direction,&QCheckBox::toggled,this,[this]{updateSettings();});connect(m_fit,&QCheckBox::toggled,this,[this]{updateSettings();});
    connect(clearButton,&QPushButton::clicked,this,&PsdPage::clear);
    connect(m_record,&QPushButton::clicked,this,[this]{auto path=QFileDialog::getSaveFileName(this,QStringLiteral("记录全部 PSD 事件"),{},QStringLiteral("PSD 事件 (*.txt)"));
        if(!path.isEmpty()){m_recordPath=path;m_record->setText(QStringLiteral("记录：%1").arg(path));}});
    connect(m_open,&QPushButton::clicked,this,[this]{auto path=QFileDialog::getOpenFileName(this,QStringLiteral("载入 PSD 事件"),{},QStringLiteral("PSD 事件 (*.txt)"));if(!path.isEmpty())m_session.replay(path);});
    connect(exportResult,&QPushButton::clicked,this,[this]{
        if(!m_snapshot)return;auto path=QFileDialog::getSaveFileName(this,QStringLiteral("导出 PSD 统计"),{},QStringLiteral("CSV (*.csv)"));if(path.isEmpty())return;
        QFile file(path);if(!file.open(QIODevice::WriteOnly)){emit errorOccurred(file.errorString());return;}
        QTextStream out(&file);const auto &s=*m_snapshot;
        out<<"# retained="<<s.retained<<"\n# config_version="<<s.configVersion<<"\n# threshold="<<s.settings.threshold<<"\n# high_is_neutron="<<s.settings.highIsNeutron
           <<"\n# roi="<<s.settings.roiMin<<','<<s.settings.roiMax<<"\n# fit_range="<<s.settings.fitMin<<','<<s.settings.fitMax<<"\n# fom="<<s.fom<<"\n";
        for(int i=0;i<s.fit.peaks.size();++i) {
            const auto &peak=s.fit.peaks[i];double bin=(s.settings.rMax-s.settings.rMin)/512;
            out<<"# fitted_peak_"<<i<<"_mean="<<s.settings.rMin+(peak.centroid+.5)*bin
               <<",fwhm="<<peak.fwhm*bin<<",area="<<peak.area<<'\n';
        }
        out<<"q_bin_center,neutron_candidates,gamma_candidates\n";
        for(int i=0;i<1024;++i)out<<s.settings.xMin+(i+.5)*(s.settings.xMax-s.settings.xMin)/1024<<','<<s.neutron[i]<<','<<s.gamma[i]<<'\n';
        out<<"r_bin_center,roi_counts\n";for(int i=0;i<512;++i)out<<s.settings.rMin+(i+.5)*(s.settings.rMax-s.settings.rMin)/512<<','<<s.projection[i]<<'\n';
        out.flush();if(!file.flush())emit errorOccurred(file.errorString());
    });
    connect(&m_session,&PsdSession::snapshotReady,this,[this](std::shared_ptr<const PsdSnapshot> s){
        if(s->settings.revision!=m_settings.revision)return;m_snapshot=s;m_plot->setSnapshot(s);
        m_status->setText(QStringLiteral("最近 %1 / 本批接收 %2 条；配置 %3；排除计数（可重叠）：pileup %4、ADC overrange %5、电荷异常 %6、Q≤0 %7；显示范围外 %8；ROI %9 条；FOM：%10（事件版本 %11）")
            .arg(s->retained).arg(s->received).arg(s->configVersion).arg(s->pileup).arg(s->overrange).arg(s->arithmetic).arg(s->nonpositive).arg(s->outside).arg(s->roiEvents)
            .arg(std::isfinite(s->fom)?QString::number(s->fom,'f',4):QStringLiteral("不可用 / 未拟合")).arg(s->fitDataVersion));
    });
    connect(&m_session,&PsdSession::errorOccurred,this,&PsdPage::errorOccurred);updateSettings();
}
ChannelConfig PsdPage::channelConfig(ChannelConfig c) const {
    c.integrationPostSamples=quint16(qRound(m_total->value()/.008));c.integrationPreSamples=quint16(qRound(m_pre->value()/.008));
    c.psdTailStart=qint16(qRound(m_start->value()/.008));c.psdTailEnd=qint16(qRound(m_end->value()/.008));return c;
}
bool PsdPage::validate(QString *error) const {
    auto c=channelConfig({});int n=c.integrationPostSamples,p=c.integrationPreSamples;
    if(n<6||n>1250||p>=n||c.psdTailStart < -p||c.psdTailEnd>n-p||c.psdTailStart>=c.psdTailEnd){
        if(error)*error=QStringLiteral("PSD 窗口要求 6≤N≤1250、P<N、-P≤尾部起点<终点≤N-P");return false;}return true;
}
void PsdPage::updateSettings() {
    if(m_xmax->value()<=m_xmin->value()||m_rmax->value()<=m_rmin->value()||m_roiMax->value()<=m_roiMin->value()||m_fitMax->value()<=m_fitMin->value()){m_status->setText(QStringLiteral("上限必须大于下限；当前输入尚未应用，图谱保留此前有效设置。"));return;}
    m_settings.xMin=m_xmin->value();m_settings.xMax=m_xmax->value();m_settings.rMin=m_rmin->value();m_settings.rMax=m_rmax->value();
    m_settings.roiMin=m_roiMin->value();m_settings.roiMax=m_roiMax->value();m_settings.fitMin=m_fitMin->value();m_settings.fitMax=m_fitMax->value();
    m_settings.threshold=m_threshold->value();m_settings.highIsNeutron=m_direction->isChecked();m_settings.fitEnabled=m_fit->isChecked();
    ++m_settings.revision;m_session.setSettings(m_settings);
}
void PsdPage::begin(const ChannelConfig &c,bool filtered){m_session.begin(c,filtered,m_recordPath);}
void PsdPage::addEvents(const QVector<PsdEvent> &e){m_session.addEvents(e);}
void PsdPage::setRunning(bool running){
    if(m_running && !running)m_session.end();m_running=running;
    for(auto b:{m_total,m_pre,m_start,m_end})b->setEnabled(!running);m_record->setEnabled(!running);m_open->setEnabled(!running);
}
void PsdPage::clear(){m_session.clear();}
void PsdPage::setTransportStatus(quint64 lost,quint64 dropped){m_transport->setText(QStringLiteral("UDP 丢包：%1；FPGA FIFO 丢弃：%2；分析缓存最多最近 100 万条，记录文件保存全部收到事件。").arg(lost).arg(dropped));}
}
