// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 SnapRYU contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "performance_overlay.h"
#include "process_utilization.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <QCloseEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QShowEvent>
#include "common/settings.h"
#include "common/lsfg_runtime.h"
#include "core/perf_stats.h"
#include "main_window.h"
#include "video_core/shader_notify.h"

namespace {
constexpr int Padding = 6;
constexpr int Margin = 8;
constexpr int LineHeight = 17;
constexpr int SpinnerRowHeight = 24;
constexpr int SpinnerSpokes = 12;
constexpr int SpinnerStepMs = 80; // one turn per ~1 s
constexpr int SpinnerAlphaStep = 19; // head 255, last trailing spoke 46
QString Number(double value) {
    return std::isfinite(value) && value >= 0.0 && value < 10000.0
               ? QString::number(value, 'f', 2) : QStringLiteral("--");
}
QString Percent(double value) {
    return std::isfinite(value) && value >= 0.0
               ? QStringLiteral("%1%").arg(value, 0, 'f', 2) : QStringLiteral("--");
}
QString Megabytes(std::int64_t bytes) {
    return bytes >= 0 ? QStringLiteral("%1MB").arg(bytes / 1'000'000) : QStringLiteral("--");
}
} // namespace

PerformanceOverlay::PerformanceOverlay(MainWindow* parent, QWidget* anchor)
    : QWidget(parent), m_mainWindow{parent}, m_anchor{anchor ? anchor : parent} {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowDoesNotAcceptFocus);
    QFont overlay_font{QStringLiteral("Segoe UI")};
    overlay_font.setPixelSize(12);
    setFont(overlay_font);
    setCursor(Qt::SizeAllCursor);
    setAccessibleName(tr("Performance overlay"));
    setToolTip(tr("FPS: accepted Vulkan presentation requests, including generated frames; "
                  "not a monitor scanout measurement. LSFG GPU appears only when generated "
                  "presents were counted in the sample.\n"
                  "Native: game-rendered FPS before frame generation; shown only when LSFG is enabled.\n"
                  "CPU: process CPU time divided by total logical CPU capacity.\n"
                  "GPU: this process's busiest Windows GPU engine.\n"
                  "RAM: process resident working set. VRAM: driver-estimated device-local "
                  "usage / current budget, not physical capacity. MB = 1,000,000 bytes.\n"
                  "CPU/GPU/RAM sampled about once per second; -- means unavailable or warming up.\n"
                  "Mode: active CPU backend.\n"
                  "Shaders: shaders built since the spinner appeared; the spinner turns "
                  "while shaders are being built. Drag to move."));
    m_spin_timer.setInterval(SpinnerStepMs);
    connect(&m_spin_timer, &QTimer::timeout, this, [this] {
        m_spin_step = (m_spin_step + 1) % SpinnerSpokes;
        update();
    });
    if (m_anchor) m_anchor->installEventFilter(this);
    updateLines(std::numeric_limits<double>::quiet_NaN());
    if (parent) {
        connect(parent, &MainWindow::positionChanged, this, &PerformanceOverlay::resetPosition);
        // Reuse the existing 500 ms UI sample; never reset core stats a second time.
        connect(parent, &MainWindow::statsUpdated, this, &PerformanceOverlay::updateStats);
    }
}

PerformanceOverlay::~PerformanceOverlay() {
    stopSampling();
}

bool PerformanceOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_anchor && (event->type() == QEvent::Resize ||
                               event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        resetPosition();
    }
    return QWidget::eventFilter(watched, event);
}

void PerformanceOverlay::showEvent(QShowEvent* event) {
    m_present_sampler.Reset();
    m_fps = QStringLiteral("--");
    m_npu_fg = false;
    m_shaders_last_started = -1;
    m_shader_burst_base = -1;
    m_shader_burst = 0;
    if (!m_sampling) {
        Frontend::ProcessUtilization::Instance().AddClient();
        m_sampling = true;
    }
    updateLines(std::numeric_limits<double>::quiet_NaN());
    QWidget::showEvent(event);
}

void PerformanceOverlay::hideEvent(QHideEvent* event) {
    stopSampling();
    QWidget::hideEvent(event);
}

void PerformanceOverlay::stopSampling() {
    m_present_sampler.Reset();
    m_spin_timer.stop();
    if (m_sampling) {
        Frontend::ProcessUtilization::Instance().RemoveClient();
        m_sampling = false;
    }
}

void PerformanceOverlay::resetPosition(const QPoint&) {
    QWidget* anchor = m_anchor ? m_anchor.data() : m_mainWindow;
    if (!anchor) return;
    const auto origin = anchor->mapToGlobal(QPoint{});
    const QPoint desired{anchor->width() - width() - Margin + m_drag_offset.x(),
                         Margin + m_drag_offset.y()};
    const QPoint bounded{std::clamp(desired.x(), 0, std::max(0, anchor->width() - width())),
                         std::clamp(desired.y(), 0, std::max(0, anchor->height() - height()))};
    move(origin + bounded);
}

void PerformanceOverlay::UpdateRendererStats(const VideoCore::RendererPerformanceStats& stats) {
    if (!m_sampling) return;
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto rates = m_present_sampler.Sample(stats.presents, now);
    m_fps = Number(rates.total);
    m_npu_fg = rates.generated > 0.0;
    m_vram = stats.memory_available
                 ? QStringLiteral("%1MB / %2MB").arg(stats.memory_used / 1'000'000)
                                                   .arg(stats.memory_budget / 1'000'000)
                 : QStringLiteral("--");
}

void PerformanceOverlay::updateStats(const Core::PerfStatsResults& results,
                                    const VideoCore::ShaderNotify& shaders) {
    const int completed = shaders.ShadersCompleted();
    const int started = shaders.ShadersStarted();
    // First sample, or a new game reset the counters: only shaders still in flight count.
    if (m_shaders_last_started < 0 || started < m_shaders_last_started) {
        m_shaders_last_started = completed;
        m_shader_burst_base = -1;
    }
    // Building = a shader is in flight now, or one started since the last 500 ms sample.
    const bool building = started > completed || started != m_shaders_last_started;
    if (building && m_shader_burst_base < 0) {
        m_shader_burst_base = m_shaders_last_started;
    } else if (!building) {
        m_shader_burst_base = -1;
    }
    m_shaders_last_started = started;
    m_shader_burst = building ? started - m_shader_burst_base : 0;
    if (building && m_sampling) {
        if (!m_spin_timer.isActive()) m_spin_timer.start();
    } else {
        m_spin_timer.stop();
    }
    updateLines(results.average_game_fps);
}

void PerformanceOverlay::updateLines(double native_fps) {
    const auto sample = m_sampling ? Frontend::ProcessUtilization::Instance().Read()
                                  : Frontend::ProcessUtilizationSnapshot{};
    const auto mode = Settings::IsNceEnabled() ? QStringLiteral("NCE") : QStringLiteral("Dynamic");
    const bool npu_enabled = Settings::values.frame_gen.GetValue();
    const bool npu_paused = npu_enabled && Settings::LsfgRuntime::paused.load(std::memory_order_relaxed);
    m_lines = {QStringLiteral("FPS: %1%2").arg(
        m_fps, npu_paused                ? QStringLiteral(" (LSFG paused)")
               : npu_enabled && m_npu_fg ? QStringLiteral(" (LSFG GPU)")
                                         : QString{})};
    if (npu_enabled && !npu_paused) {
        m_lines.append(QStringLiteral("Native: %1 FPS").arg(Number(native_fps)));
    }
    m_lines.append({QStringLiteral("CPU: %1").arg(Percent(sample.cpu_percent)),
               QStringLiteral("GPU: %1").arg(Percent(sample.gpu_percent)),
               QStringLiteral("RAM: %1").arg(Megabytes(sample.ram_bytes)),
               QStringLiteral("VRAM: %1").arg(m_vram),
               QStringLiteral("Mode: %1").arg(mode)});
    const QFontMetrics metrics{font()};
    int panel_width = 180;
    for (const auto& line : m_lines) {
        panel_width = std::max(panel_width, metrics.horizontalAdvance(line) + Padding * 2);
    }
    int panel_height = Padding * 2 + LineHeight * static_cast<int>(m_lines.size());
    if (m_shader_burst > 0) {
        // The spinner row is drawn below the text lines, not kept in m_lines.
        const QString shader_line = QStringLiteral("Shaders: %1").arg(m_shader_burst);
        panel_width = std::max(panel_width, SpinnerRowHeight + Padding +
                                                metrics.horizontalAdvance(shader_line) +
                                                Padding * 2);
        panel_height += SpinnerRowHeight;
    }
    setFixedSize(panel_width, panel_height);
    setAccessibleDescription(m_lines.join(QStringLiteral(", ")));
    resetPosition();
    update();
}

void PerformanceOverlay::paintEvent(QPaintEvent*) {
    QPainter painter{this};
    painter.fillRect(rect(), QColor{0, 0, 0, 165});
    painter.setPen(Qt::white);
    painter.setFont(font());
    for (int i = 0; i < m_lines.size(); ++i) {
        painter.drawText(QRect{Padding, Padding + i * LineHeight,
                               width() - Padding * 2, LineHeight},
                         Qt::AlignLeft | Qt::AlignVCenter, m_lines[i]);
    }
    if (m_shader_burst > 0) {
        const int top = Padding + static_cast<int>(m_lines.size()) * LineHeight;
        drawShaderSpinner(painter, QRectF(Padding, top, SpinnerRowHeight, SpinnerRowHeight));
        const int text_left = Padding + SpinnerRowHeight + Padding;
        painter.setPen(Qt::white);
        painter.drawText(QRect{text_left, top, width() - text_left - Padding, SpinnerRowHeight},
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Shaders: %1").arg(m_shader_burst));
    }
}

void PerformanceOverlay::drawShaderSpinner(QPainter& painter, const QRectF& area) const {
    const double outer = area.height() / 2.0 - 2.0;
    const double inner = outer * 0.5;
    QPen pen;
    pen.setWidthF(std::max(2.0, outer * 0.24));
    pen.setCapStyle(Qt::RoundCap);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(area.center());
    for (int i = 0; i < SpinnerSpokes; ++i) {
        // Spoke i sits i*30 degrees clockwise from 12 o'clock. The head (spoke m_spin_step)
        // is opaque; spokes behind it fade, so the bright spoke appears to turn clockwise.
        const int age = (m_spin_step - i + SpinnerSpokes) % SpinnerSpokes;
        pen.setColor(QColor{255, 255, 255, 255 - age * SpinnerAlphaStep});
        painter.setPen(pen);
        painter.drawLine(QPointF{0.0, -inner}, QPointF{0.0, -outer});
        painter.rotate(360.0 / SpinnerSpokes);
    }
    painter.restore();
}

void PerformanceOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_drag_start_pos = event->position().toPoint();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PerformanceOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        QWidget* anchor = m_anchor ? m_anchor.data() : m_mainWindow;
        if (anchor) {
            const QPoint desired = event->globalPosition().toPoint() - m_drag_start_pos;
            const QPoint default_position = anchor->mapToGlobal(
                QPoint{anchor->width() - width() - Margin, Margin});
            m_drag_offset = desired - default_position;
            resetPosition();
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void PerformanceOverlay::closeEvent(QCloseEvent* event) {
    stopSampling();
    emit closed();
    event->accept();
}
