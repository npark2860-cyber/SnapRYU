// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QTimer>
#include "common/npu_fg_settings.h"
#include <qnamespace.h>
#include "common/settings.h"
#include "core/core.h"
#include "qt_common/config/shared_translation.h"
#include "ui_configure_graphics_advanced.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics_advanced.h"
#include "yuzu/configuration/shared_widget.h"

ConfigureGraphicsAdvanced::ConfigureGraphicsAdvanced(
    const Core::System& system_, std::shared_ptr<std::vector<ConfigurationShared::Tab*>> group_,
    const ConfigurationShared::Builder& builder, QWidget* parent)
    : Tab(group_, parent), ui{std::make_unique<Ui::ConfigureGraphicsAdvanced>()}, system{system_} {

    ui->setupUi(this);

    Setup(builder);

    SetConfiguration();

    checkbox_enable_compute_pipelines->setVisible(false);
}

ConfigureGraphicsAdvanced::~ConfigureGraphicsAdvanced() = default;

void ConfigureGraphicsAdvanced::SetConfiguration() {
    const bool global = Settings::IsConfiguringGlobal();
    auto* enabled = findChild<QCheckBox*>("lsfgArm64Enabled");
    auto* inherit = findChild<QCheckBox*>("lsfgInherit");
    auto* fp16 = findChild<QCheckBox*>("lsfgFp16");
    auto* flow_auto = findChild<QCheckBox*>("lsfgFlowAuto");
    auto* flow = findChild<QSpinBox*>("lsfgFlowScale");
    if (!enabled || !inherit || !fp16 || !flow_auto || !flow) return;
    enabled->setChecked(Settings::values.frame_gen.GetValue(global));
    fp16->setChecked(Settings::values.frame_gen_fp16.GetValue(global));
    flow_auto->setChecked(Settings::values.frame_gen_flow_scale_auto.GetValue(global));
    flow->setValue(static_cast<int>(Settings::values.frame_gen_flow_scale.GetValue(global)));
    inherit->setChecked(!global && Settings::values.frame_gen.UsingGlobal());
}

void ConfigureGraphicsAdvanced::Setup(const ConfigurationShared::Builder& builder) {
    auto& normal_layout = *ui->normal_target->layout();

    auto* fg = new QGroupBox(tr("LSFG — ARM64"), this);
#ifndef HAS_SNAPRYU_LSFG_ARM64
    fg->setVisible(false);
#endif
    auto* layout = new QVBoxLayout(fg);
    auto* inherit = new QCheckBox(tr("Use global settings"), fg);
    inherit->setObjectName("lsfgInherit");
    inherit->setVisible(!Settings::IsConfiguringGlobal());
    layout->addWidget(inherit);
    auto* enabled = new QCheckBox(tr("Enable LSFG 2X (GPU)"), fg);
    enabled->setObjectName("lsfgArm64Enabled");
    layout->addWidget(enabled);
    // Eden's own frame_gen_* settings (hidden on desktop by shared_widget.cpp IsAndroidOnly);
    // vk_lsfg_arm64.cpp reads them when the game starts.
    auto* fp16 = new QCheckBox(tr("FP16 shaders (faster; FP32 if the GPU has no FP16)"), fg);
    fp16->setObjectName("lsfgFp16");
    layout->addWidget(fp16);
    auto* flow_auto = new QCheckBox(tr("Automatic flow scale (rendered width / window width)"), fg);
    flow_auto->setObjectName("lsfgFlowAuto");
    layout->addWidget(flow_auto);
    auto* flow_row = new QHBoxLayout();
    auto* flow_label = new QLabel(tr("Flow scale"), fg);
    auto* flow = new QSpinBox(fg);
    flow->setObjectName("lsfgFlowScale");
    flow->setRange(25, 100);
    flow->setSingleStep(5);
    flow->setSuffix(QStringLiteral("%"));
    flow_row->addWidget(flow_label);
    flow_row->addWidget(flow);
    flow_row->addStretch();
    layout->addLayout(flow_row);
    auto* info = new QLabel(tr("Uses the ARM64 LSFG port and the original Lossless.dll shaders. "
        "Quality mode, full output resolution. A lower flow scale is faster but tracks motion "
        "less precisely; automatic uses the same rule as Eden's LSFG. NPU/Anvil is not used. "
        "F10 pauses or resumes LSFG; Shift+F10 changes docked mode. "
        "Close the game to change these settings. Copy Lossless.dll and lsfg_arm64_v2.dll next to "
        "SnapRYU.exe; no other location is searched."), fg);
    info->setWordWrap(true);
    layout->addWidget(info);
    const auto update = [=] {
        fg->setEnabled(!system.IsPoweredOn() && !Settings::NpuSession::Busy());
        const bool own = !inherit->isChecked();
        enabled->setEnabled(own);
        fp16->setEnabled(own);
        flow_auto->setEnabled(own);
        flow->setEnabled(own && !flow_auto->isChecked());
        flow_label->setEnabled(flow->isEnabled());
    };
    connect(inherit, &QCheckBox::toggled, fg, [=](bool checked) {
        if (checked) {
            enabled->setChecked(Settings::values.frame_gen.GetValue(true));
            fp16->setChecked(Settings::values.frame_gen_fp16.GetValue(true));
            flow_auto->setChecked(Settings::values.frame_gen_flow_scale_auto.GetValue(true));
            flow->setValue(static_cast<int>(Settings::values.frame_gen_flow_scale.GetValue(true)));
        }
        update();
    });
    connect(flow_auto, &QCheckBox::toggled, fg, [=](bool) { update(); });
    auto* timer = new QTimer(fg); timer->setInterval(100);
    connect(timer, &QTimer::timeout, fg, update); timer->start();
    update();
    apply_funcs.push_back([=](bool powered_on) {
        if (powered_on || Settings::NpuSession::Busy()) return;
        const bool global = Settings::IsConfiguringGlobal();
        const bool use_global = global || inherit->isChecked();
        // One "Use global settings" box covers all four LSFG settings.
        const auto store = [&](auto& setting, auto value) {
            setting.SetGlobal(use_global);
            if (global || !use_global) setting.SetValue(value);
        };
        store(Settings::values.frame_gen, enabled->isChecked());
        store(Settings::values.frame_gen_fp16, fp16->isChecked());
        store(Settings::values.frame_gen_flow_scale_auto, flow_auto->isChecked());
        store(Settings::values.frame_gen_flow_scale, static_cast<u32>(flow->value()));
    });
    normal_layout.addWidget(fg);

    // A map will sort the data for us
    std::map<u32, QWidget*> normal_hold{};

    for (auto setting :
         Settings::values.linkage.by_category[Settings::Category::RendererAdvanced]) {
        ConfigurationShared::Widget* widget = builder.BuildWidget(setting, apply_funcs);

        if (widget == nullptr) {
            continue;
        }
        if (!widget->Valid()) {
            widget->deleteLater();
            continue;
        }

        const auto id = setting->Id();

        normal_hold.emplace(id, widget);

        // Keep track of enable_compute_pipelines so we can display it when needed
        if (setting->Id() == Settings::values.enable_compute_pipelines.Id()) {
            checkbox_enable_compute_pipelines = widget;
        }
    }

    for (const auto& [id, widget] : normal_hold) {
        normal_layout.addWidget(widget);
    }
}

void ConfigureGraphicsAdvanced::ApplyConfiguration() {
    const bool is_powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(is_powered_on);
    }
}

void ConfigureGraphicsAdvanced::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphicsAdvanced::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureGraphicsAdvanced::ExposeComputeOption() {
    checkbox_enable_compute_pipelines->setVisible(true);
}
