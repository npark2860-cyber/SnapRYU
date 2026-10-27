// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuzu/snap_go_dialog.h"
#include "yuzu/snap_go_mode_view.h"
#include <algorithm>
#include <filesystem>
#include <optional>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include "common/fs/path_util.h"
#include "common/snapryu_go.h"

namespace {
QString profile_path;
bool pause_gate{}, ui_gate_owned{}, previous_gate{};
qsizetype modal_count{};
void UpdateInputGate() {
    const bool needed=pause_gate||modal_count>0;
    if(needed&&!ui_gate_owned){previous_gate=SnapGo::IsInputBlocked();ui_gate_owned=true;SnapGo::SetInputBlocked(true);}
    else if(!needed&&ui_gate_owned){ui_gate_owned=false;SnapGo::SetInputBlocked(previous_gate);}
}
bool PrepareProfileDirectory(){return QDir().mkpath(profile_path);}
bool IsPhoneAddress(const QString& address) {
    return !address.isEmpty() && !address.startsWith(QStringLiteral("127.")) &&
           address != QStringLiteral("::1") && address != QStringLiteral("0.0.0.0");
}
QStringList PhoneAddresses() {
    QStringList result;
    for (const auto& value : SnapGo::ListAddresses()) {
        const auto address = QString::fromStdString(value);
        if (IsPhoneAddress(address) && !result.contains(address)) result.push_back(address);
    }
    return result;
}

// A desktop focus change is not an input takeover. Only visible modal windows
// gate gameplay; nested dialogs restore the prior gate rather than clearing it.
class ModalInputGate final : public QObject {
public:
    explicit ModalInputGate(QObject* parent):QObject(parent){}
protected:
    bool eventFilter(QObject* watched,QEvent* event) override {
        if(event->type()==QEvent::Show) {
            auto* widget=qobject_cast<QWidget*>(watched);
            if(widget&&widget->isWindow()&&widget->windowModality()!=Qt::NonModal&&!modals.contains(watched)) {
                modals.insert(watched);
                modal_count=modals.size();UpdateInputGate();
            }
        }else if(event->type()==QEvent::Hide||event->type()==QEvent::Destroy) {
            if(modals.remove(watched)){modal_count=modals.size();UpdateInputGate();}
        }
        return QObject::eventFilter(watched,event);
    }
private:
    QSet<QObject*> modals;
};
}

class SnapGoQrWidget final : public QWidget {
public:
    explicit SnapGoQrWidget(QWidget* parent):QWidget(parent){
        setMinimumSize(260,260);setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0,0,0,0);
        idle = new SnapGoModeView(SnapGoModeView::Style::PairingIdle, this);
        layout->addWidget(idle);
    }
    void Set(SnapGo::PairingGrid next){grid=std::move(next);idle->hide();update();}
    void Clear(){grid.reset();idle->show();update();}
    void SetConnected(bool connected){idle->SetWaitingConnected(connected);}
    QSize sizeHint() const override{return {380,380};}
protected:
    void paintEvent(QPaintEvent*) override {
        if(!grid)return;
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing,false);
        painter.fillRect(rect(),Qt::white);
        const int side=static_cast<int>(grid->side),cells=side+8;
        const int scale=(std::min)(width(),height())/cells;
        if(scale<1)return;
        const int left=(width()-cells*scale)/2,top=(height()-cells*scale)/2;
        // Four white modules on every edge, with integer-sized black modules.
        for(int y=0;y<side;++y)for(int x=0;x<side;++x)
            if(grid->modules[static_cast<size_t>(y)*grid->side+x])
                painter.fillRect(left+(x+4)*scale,top+(y+4)*scale,scale,scale,Qt::black);
    }
private:
    std::optional<SnapGo::PairingGrid> grid;
    SnapGoModeView* idle{};
};

namespace SnapGoUi {
QString SettingsPath(){return QDir(profile_path).filePath(QStringLiteral("snapryugo-ui.ini"));}
void SetGamePaused(bool paused){
    pause_gate=paused;UpdateInputGate();
    if(paused){SnapGo::SetRumble(0,0,0);SnapGo::SetRumble(1,0,0);}
}
void Initialize() {
#ifdef _WIN32
    const auto appdata=qEnvironmentVariable("APPDATA");
    if(!appdata.isEmpty()&&QDir::isAbsolutePath(appdata))profile_path=QDir(appdata).filePath(QStringLiteral("SnapRYU"));
#endif
    if(profile_path.isEmpty())profile_path=QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::EdenDir));
    profile_path=QDir(profile_path).absolutePath();
    const auto dll=QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("snapryu_go_host.dll"));
    SnapGo::Initialize(std::filesystem::path(profile_path.toStdWString()),std::filesystem::path(dll.toStdWString()));
    auto* modal_gate=new ModalInputGate(qApp);qApp->installEventFilter(modal_gate);
    QSettings settings(SettingsPath(),QSettings::IniFormat);
    SnapGo::SetCompactRequested(settings.value(QStringLiteral("compact"),false).toBool());
    if(settings.value(QStringLiteral("enabled"),false).toBool()) {
        auto bind=settings.value(QStringLiteral("bind"),QString{}).toString();
        const auto available = PhoneAddresses();
        if (!available.contains(bind)) bind = available.size() == 1 ? available.front() : QString{};
        if(IsPhoneAddress(bind)&&PrepareProfileDirectory()&&SnapGo::InitializeIdentity()&&
           SnapGo::SetBindAddress(bind.toStdString())&&SnapGo::SetEnabled(true)) {
            const auto current = SnapGo::GetStatus();
            settings.setValue(QStringLiteral("bind"), QString::fromUtf8(current.bind_address));
            settings.sync();
        }
    }
}
}

SnapGoDialog::SnapGoDialog(QWidget* parent):QDialog(parent) {
    setWindowTitle(QStringLiteral("SnapRYU Go mode"));setWindowModality(Qt::WindowModal);resize(620,780);
    setObjectName(QStringLiteral("snapGoDialog"));
    setStyleSheet(QStringLiteral(
        "QDialog#snapGoDialog { background: #20252B; }"
        "QLabel, QCheckBox { color: #F3F6FA; }"
        "QComboBox, QPushButton { background: #292F36; color: #F3F6FA; border: 1px solid #48535F; border-radius: 5px; padding: 6px; }"
        "QPushButton:hover { border-color: #00A8FF; }"
        "QPushButton:disabled, QComboBox:disabled, QCheckBox:disabled { color: #ADB7C2; }"
        "QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #ADB7C2; border-radius: 3px; background: #292F36; }"
        "QCheckBox::indicator:checked { background: #00A8FF; border-color: #00A8FF; }"));
    auto* layout=new QVBoxLayout(this);
    auto* intro=new QLabel(tr("같은 집 네트워크에서 휴대폰으로 게임 화면·소리·컨트롤러를 연결합니다.\n영상은 720p이며 휴대폰에서 30 / 60fps를 선택할 수 있습니다."),this);
    intro->setWordWrap(true);layout->addWidget(intro);
    enabled=new QCheckBox(tr("휴대폰 연결 켜기"),this);layout->addWidget(enabled);
    auto* row=new QHBoxLayout;row->addWidget(new QLabel(tr("PC 주소"),this));address=new QComboBox(this);row->addWidget(address,1);
    auto* refresh=new QPushButton(tr("주소 새로고침"),this);row->addWidget(refresh);layout->addLayout(row);
    layout->addWidget(new QLabel(tr("연결 포트: 26762 (고정)"),this));
    compact=new QCheckBox(tr("Compact 모드 — 게임 시작 시 1× Docked 적용"),this);layout->addWidget(compact);
    auto* compact_note=new QLabel(tr("Compact 모드는 게임을 종료한 상태에서 선택합니다. 해당 세션에서는 내장 젤다 왕눈 고정 1080p 패치를 적용하지 않으며 저장된 옵션은 유지합니다. 연결 중에는 합성 화면과 LSFG를 1280×720으로 처리하고 PC 게임 표시를 쉽니다. 연결이 끊기면 PC 표시가 돌아옵니다. 게임 내부 해상도는 게임이 결정하며 720p로 고정하지 않습니다."),this);
    compact_note->setWordWrap(true);layout->addWidget(compact_note);
    auto* actions=new QHBoxLayout;
    auto* identity=new QPushButton(tr("PC 연결 인증서 준비"),this);
    pair=new QPushButton(tr("페어링 QR 표시"),this);
    auto* reset=new QPushButton(tr("페어링 초기화"),this);
    actions->addWidget(identity);actions->addWidget(pair);actions->addWidget(reset);layout->addLayout(actions);
    qr=new SnapGoQrWidget(this);layout->addWidget(qr,1);
    qr_hint=new QLabel(tr("연결을 켠 뒤 QR을 표시하고 휴대폰에서 스캔하세요."),this);qr_hint->setWordWrap(true);layout->addWidget(qr_hint);
    status=new QLabel(this);status->setTextFormat(Qt::PlainText);status->setWordWrap(true);layout->addWidget(status);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,this);connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);layout->addWidget(buttons);
    compact->setChecked(SnapGo::IsCompactRequested());RefreshAddresses();RefreshStatus();
    connect(refresh,&QPushButton::clicked,this,[this]{RefreshAddresses();RefreshStatus();});
    connect(enabled,&QCheckBox::toggled,this,[this](bool value){ApplyEnabled(value);});
    connect(compact,&QCheckBox::toggled,this,[this](bool value){SnapGo::SetCompactRequested(value);SavePreferences();});
    connect(address,&QComboBox::currentTextChanged,this,[this](const QString& value){
        if(IsPhoneAddress(value)&&!SnapGo::GetStatus().enabled&&SnapGo::SetBindAddress(value.toStdString()))SavePreferences();
        RefreshStatus();
    });
    connect(identity,&QPushButton::clicked,this,[this]{if(!PrepareProfileDirectory()||!SnapGo::InitializeIdentity())ShowFailure(tr("인증서를 준비하지 못했습니다."));RefreshStatus();});
    connect(pair,&QPushButton::clicked,this,[this]{
        ClearQr();
        if(!SnapGo::GetStatus().enabled&&!ApplyEnabled(true))return;
        if(auto grid=SnapGo::OpenPairing()){
            qr->Set(std::move(*grid));qr_hint->setText(tr("휴대폰에서 QR을 스캔하세요. 이 창을 닫으면 QR이 취소됩니다."));qr_timer.start(120000);
        }else ShowFailure(tr("페어링 QR을 만들지 못했습니다."));
    });
    connect(reset,&QPushButton::clicked,this,[this]{
        if(QMessageBox::question(this,QStringLiteral("SnapRYU Go!"),tr("등록된 휴대폰 페어링을 초기화할까요? 휴대폰에서 다시 페어링해야 합니다."))!=QMessageBox::Yes)return;
        ClearQr();if(!SnapGo::ResetPairing())ShowFailure(tr("페어링을 초기화하지 못했습니다."));RefreshStatus();
    });
    qr_timer.setParent(this);qr_timer.setObjectName(QStringLiteral("snapGoPairingExpiry"));
    qr_timer.setSingleShot(true);connect(&qr_timer,&QTimer::timeout,this,[this]{ClearQr();qr_hint->setText(tr("QR 표시 시간이 끝났습니다. 새 QR을 표시해 주세요."));});
    connect(&status_timer,&QTimer::timeout,this,[this]{RefreshStatus();});status_timer.start(500);
}
SnapGoDialog::~SnapGoDialog(){ClearQr();}
void SnapGoDialog::ClearQr(){qr_timer.stop();SnapGo::CancelPairing();if(qr)qr->Clear();}
void SnapGoDialog::ShowFailure(const QString& fallback){
    const auto current=SnapGo::GetStatus();QString message=fallback;
    if(current.message[0])message+=QStringLiteral("\n\n")+QString::fromUtf8(current.message);
    QMessageBox::warning(this,QStringLiteral("SnapRYU Go!"),message);
}
void SnapGoDialog::RefreshAddresses(){
    const QSignalBlocker blocked(address);
    QSettings settings(SnapGoUi::SettingsPath(),QSettings::IniFormat);
    const auto current=SnapGo::GetStatus();
    const auto active = QString::fromUtf8(current.bind_address);
    const auto selected=current.enabled&&IsPhoneAddress(active)?active
        :address->currentText().isEmpty()?settings.value(QStringLiteral("bind"),QString{}).toString():address->currentText();
    address->clear();address->addItems(PhoneAddresses());
    auto index=address->findText(selected);
    if(current.enabled&&IsPhoneAddress(active)&&index<0){address->addItem(active);index=address->count()-1;}
    if(selected.isEmpty()&&address->count()==1)index=0;
    address->setCurrentIndex(index);
}
void SnapGoDialog::SavePreferences(){
    QSettings settings(SnapGoUi::SettingsPath(),QSettings::IniFormat);
    const auto current=SnapGo::GetStatus();
    settings.setValue(QStringLiteral("enabled"),current.enabled!=0);
    settings.setValue(QStringLiteral("bind"),current.enabled&&current.bind_address[0]?QString::fromUtf8(current.bind_address):address->currentText());
    settings.setValue(QStringLiteral("compact"),SnapGo::IsCompactRequested());settings.sync();
}
bool SnapGoDialog::ApplyEnabled(bool value){
    bool ok=true;
    if(value){
        if(!IsPhoneAddress(address->currentText())){
            QMessageBox::warning(this,QStringLiteral("SnapRYU Go!"),tr("PC를 휴대폰과 같은 Wi-Fi 또는 유선 네트워크에 연결한 뒤 주소 새로고침을 누르고 PC 주소를 선택하세요."));
            RefreshStatus();return false;
        }
        else ok=PrepareProfileDirectory()&&SnapGo::InitializeIdentity()&&SnapGo::SetBindAddress(address->currentText().toStdString())&&SnapGo::SetEnabled(true);
    }else{ClearQr();ok=SnapGo::SetEnabled(false);}
    if(!ok)ShowFailure(tr("휴대폰 연결 설정을 적용하지 못했습니다."));
    RefreshStatus();if(ok)SavePreferences();return ok;
}
void SnapGoDialog::RefreshStatus(){
    const auto current=SnapGo::GetStatus();
    qr->SetConnected(current.connected);
    {const QSignalBlocker blocked(enabled);enabled->setChecked(current.enabled!=0);}
    const bool selected = IsPhoneAddress(address->currentText());
    enabled->setEnabled(current.enabled || selected);
    pair->setEnabled(current.enabled ? IsPhoneAddress(QString::fromUtf8(current.bind_address)) : selected);
    address->setEnabled(!current.enabled&&address->count()>0);compact->setEnabled(!current.game_running);
    const QString state=current.connected?tr("휴대폰 연결됨"):current.enabled?tr("휴대폰 연결 대기"):tr("꺼짐");
    QString detail = QString::fromUtf8(current.message);
    if (!current.enabled && !selected) {
        detail += QStringLiteral("\n") + (address->count() == 0
            ? tr("사용할 PC 네트워크 주소가 없습니다. PC를 휴대폰과 같은 Wi-Fi 또는 유선 네트워크에 연결하고 주소 새로고침을 누르세요.")
            : tr("연결할 PC 네트워크 주소를 선택하세요."));
    }
    status->setText(tr("%1 · 영상 %2 · 오디오 %3\n%4")
        .arg(state).arg(static_cast<qulonglong>(current.video_frames))
        .arg(current.audio_playback?tr("재생 중"):tr("대기"))
        .arg(detail));
    if(current.connected&&qr_timer.isActive()){ClearQr();qr_hint->setText(tr("휴대폰이 연결되었습니다."));}
}
