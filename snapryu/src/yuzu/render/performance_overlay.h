// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 SnapRYU contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>
#include "video_core/presentation_stats.h"

namespace VideoCore { class ShaderNotify; }
namespace Core { struct PerfStatsResults; }
class MainWindow;
class QPainter;

class PerformanceOverlay : public QWidget {
    Q_OBJECT
public:
    explicit PerformanceOverlay(MainWindow* parent = nullptr, QWidget* anchor = nullptr);
    ~PerformanceOverlay() override;
    void UpdateRendererStats(const VideoCore::RendererPerformanceStats& stats);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void resetPosition(const QPoint& pos = {});
    void updateStats(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);
    void updateLines(double native_fps);
    void stopSampling();
    void drawShaderSpinner(QPainter& painter, const QRectF& area) const;

    MainWindow* m_mainWindow = nullptr;
    QPointer<QWidget> m_anchor;
    QPoint m_drag_offset;
    QPoint m_drag_start_pos;
    QString m_fps{QStringLiteral("--")};
    QString m_vram{QStringLiteral("--")};
    QStringList m_lines;
    VideoCore::PresentationRateSampler m_present_sampler;
    bool m_npu_fg = false;
    bool m_sampling = false;
    // Shader spinner: shown while shaders are being built. Burst = shaders started since
    // the spinner appeared. m_shaders_last_started < 0 means no sample taken yet.
    QTimer m_spin_timer;
    int m_spin_step = 0;
    int m_shaders_last_started = -1;
    int m_shader_burst_base = -1;
    int m_shader_burst = 0;

signals:
    void closed();
};
