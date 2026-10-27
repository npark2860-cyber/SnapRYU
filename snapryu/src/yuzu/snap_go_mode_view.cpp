// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuzu/snap_go_mode_view.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

SnapGoModeView::SnapGoModeView(Style style, QWidget* parent) : QWidget(parent) {
    const bool pairing = style == Style::PairingIdle;
    setObjectName(pairing ? QStringLiteral("snapGoPairingIdle") : QStringLiteral("snapGoPlayingScreen"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(QStringLiteral(
        "#snapGoPlayingScreen { background: #20252B; }"
        "#snapGoPairingIdle { background: #292F36; border-radius: 12px; }"
        "QLabel { color: #F3F6FA; background: transparent; border: none; }"
        "QLabel#snapGoTitle { font-size: %1px; font-weight: 600; }"
        "QLabel#snapGoStatus { color: #00A8FF; font-size: %2px; }"
        "QLabel#snapGoSubtitle { color: #ADB7C2; font-size: %3px; }"
        "QLabel#snapGoQuality { color: #ADB7C2; font-size: 12px; }"
        "QFrame#snapGoRedAccent { background: #F7192A; border: none; }"
        "QFrame#snapGoBlueAccent { background: #00A8FF; border: none; }")
                      .arg(pairing ? 24 : 30).arg(18).arg(15));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(32, 24, 32, 24);
    root->setSpacing(0);
    root->addStretch(1);

    auto* content = new QHBoxLayout;
    content->setSpacing(0);
    content->addStretch(1);
    auto* column = new QVBoxLayout;
    column->setSpacing(pairing ? 10 : 14);

    auto* logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("snapGoLogo"));
    const int logo_size = pairing ? 112 : 160;
    logo->setFixedSize(logo_size, logo_size);
    logo->setAlignment(Qt::AlignCenter);
    logo->setPixmap(QPixmap(QStringLiteral(":/img/snapryu.png"))
                        .scaled(logo_size, logo_size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    column->addWidget(logo, 0, Qt::AlignHCenter);

    auto* title = new QLabel(this);
    title->setObjectName(QStringLiteral("snapGoTitle"));
    title->setAccessibleName(QStringLiteral("SnapRYU Go mode"));
    const auto wordmark = QPixmap(QStringLiteral(":/img/snapryu-go-brush.png"))
        .scaledToWidth(pairing ? 320 : 420, Qt::SmoothTransformation);
    title->setPixmap(wordmark);
    title->setFixedSize(wordmark.size());
    column->addWidget(title, 0, Qt::AlignHCenter);
    auto* mode = new QLabel(QStringLiteral("mode"), this);
    mode->setObjectName(QStringLiteral("snapGoQuality"));
    column->addWidget(mode, 0, Qt::AlignHCenter);
    status = new QLabel(pairing ? tr("휴대폰 연결 대기 중") : tr("휴대폰으로 플레이 중"), this);
    status->setObjectName(QStringLiteral("snapGoStatus"));
    column->addWidget(status, 0, Qt::AlignHCenter);
    auto* subtitle = new QLabel(pairing ? tr("휴대폰과 PC를 연결합니다") : tr("게임은 PC에서 실행되고 있습니다"), this);
    subtitle->setObjectName(QStringLiteral("snapGoSubtitle"));
    subtitle->setWordWrap(true);
    subtitle->setAlignment(Qt::AlignCenter);
    column->addWidget(subtitle);

    auto* quality = new QLabel(QStringLiteral("720p · 1280 × 720"), this);
    quality->setObjectName(QStringLiteral("snapGoQuality"));
    {
        column->addSpacing(5);
        column->addWidget(quality, 0, Qt::AlignHCenter);
        auto* accents = new QHBoxLayout;
        accents->setSpacing(4);
        accents->addStretch();
        for (const auto* name : {"snapGoRedAccent", "snapGoBlueAccent"}) {
            auto* accent = new QFrame(this);
            accent->setObjectName(QString::fromLatin1(name));
            accent->setFixedSize(26, 3);
            accents->addWidget(accent);
        }
        accents->addStretch();
        column->addLayout(accents);
    }
    content->addLayout(column);
    content->addStretch(1);
    root->addLayout(content);
    root->addStretch(1);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SnapGoModeView::SetWaitingConnected(bool connected) {
    const auto text = connected ? tr("휴대폰이 연결되었습니다") : tr("휴대폰 연결 대기 중");
    if (status->text() != text) status->setText(text);
}
