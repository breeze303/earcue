import SwiftUI
import AppKit
import Foundation

// MARK: - macOS 27 / 26 glass styling

struct GlassCardBackground: ViewModifier {
    @Environment(\.colorScheme) private var colorScheme

    func body(content: Content) -> some View {
        let tint = colorScheme == .dark
            ? Color.black.opacity(0.16)
            : Color.white.opacity(0.22)
        return Group {
            if #available(macOS 26.0, *) {
                content
                    .glassEffect(.regular.tint(tint),
                                 in: RoundedRectangle(cornerRadius: 22, style: .continuous))
                    .shadow(color: .black.opacity(0.12), radius: 18, y: 8)
            } else {
                content
                    .background(.ultraThinMaterial,
                                in: RoundedRectangle(cornerRadius: 20, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 20, style: .continuous)
                            .stroke(.white.opacity(0.08), lineWidth: 1)
                    )
                    .shadow(color: .black.opacity(0.10), radius: 14, y: 6)
            }
        }
    }
}

struct GlassPill: ViewModifier {
    func body(content: Content) -> some View {
        Group {
            if #available(macOS 26.0, *) {
                content.glassEffect(.regular, in: Capsule())
            } else {
                content
                    .background(.ultraThinMaterial, in: Capsule())
                    .overlay(Capsule().stroke(.white.opacity(0.08), lineWidth: 1))
            }
        }
    }
}

struct GlassRegularButtonModifier: ViewModifier {
    func body(content: Content) -> some View {
        Group {
            if #available(macOS 26.0, *) {
                content.buttonStyle(.glass)
            } else {
                content.buttonStyle(.bordered)
            }
        }
    }
}

struct GlassProminentButtonModifier: ViewModifier {
    let tint: Color

    func body(content: Content) -> some View {
        Group {
            if #available(macOS 26.0, *) {
                content.buttonStyle(.glassProminent).tint(tint)
            } else {
                content.buttonStyle(.borderedProminent).tint(tint)
            }
        }
    }
}

// MARK: - Layout building blocks

struct Card<Content: View>: View {
    let title: String
    let systemImage: String
    let content: Content

    init(title: String, systemImage: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.systemImage = systemImage
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(title, systemImage: systemImage)
                .font(.system(.headline, design: .rounded))
                .symbolRenderingMode(.hierarchical)
            content
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .modifier(GlassCardBackground())
    }
}

struct MeterRow: View {
    let title: String
    let levelDB: Double
    let peakDB: Double

    var body: some View {
        VStack(spacing: 5) {
            HStack {
                Text(title)
                    .font(.subheadline.weight(.medium))
                Spacer()
                Text(String(format: "%.0f dB", levelDB))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

            GeometryReader { geometry in
                ZStack(alignment: .leading) {
                    Capsule()
                        .fill(Color.primary.opacity(0.08))

                    Capsule()
                        .fill(LinearGradient(
                            colors: [.green, .yellow, .red],
                            startPoint: .leading,
                            endPoint: .trailing
                        ))
                        .frame(width: normalized(levelDB) * geometry.size.width)

                    Rectangle()
                        .fill(.white.opacity(0.9))
                        .frame(width: 2)
                        .offset(x: max(0, min(geometry.size.width - 2,
                                             normalized(peakDB) * geometry.size.width)))
                }
            }
            .frame(height: 12)
        }
    }

    private func normalized(_ db: Double) -> CGFloat {
        CGFloat(min(max((db + 60.0) / 60.0, 0.0), 1.0))
    }
}

// MARK: - Cards

struct PresetCard: View {
    @ObservedObject var model: AudioModel

    var body: some View {
        Card(title: "场景", systemImage: "wand.and.stars") {
            VStack(alignment: .leading, spacing: 6) {
                Picker("场景", selection: Binding(
                    get: { model.preset },
                    set: { model.applyPreset($0) }
                )) {
                    ForEach(MonitorPreset.allCases) { preset in
                        Text(preset.title).tag(preset)
                    }
                }
                .pickerStyle(.menu)

                Text(model.presetDescription)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
    }
}

struct DevicesCard: View {
    @ObservedObject var model: AudioModel

    var body: some View {
        Card(title: "设备", systemImage: "hifispeaker.2") {
            VStack(alignment: .leading, spacing: 8) {
                Text("输入")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Picker("输入", selection: Binding(
                    get: { model.selectedInputID },
                    set: { model.selectInput($0) }
                )) {
                    ForEach(model.inputDevices) { device in
                        Text(device.name).tag(device.id)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(maxWidth: .infinity)
                .disabled(model.inputDevices.isEmpty)

                Text("输出")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Picker("输出", selection: Binding(
                    get: { model.selectedOutputID },
                    set: { model.selectOutput($0) }
                )) {
                    ForEach(model.outputDevices) { device in
                        Text(device.name).tag(device.id)
                    }
                }
                .labelsHidden()
                .pickerStyle(.menu)
                .frame(maxWidth: .infinity)
                .disabled(model.outputDevices.isEmpty)

                HStack {
                    Button {
                        model.refreshDevices()
                    } label: {
                        Label("刷新设备", systemImage: "arrow.clockwise")
                    }
                    .modifier(GlassRegularButtonModifier())

                    Spacer()

                    if let output = model.selectedOutput {
                        Text("\(Int(output.sampleRate)) Hz")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                if model.inputDevices.isEmpty || model.outputDevices.isEmpty {
                    Label("未找到音频设备", systemImage: "exclamationmark.triangle")
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            }
        }
    }
}

struct BufferCard: View {
    @ObservedObject var model: AudioModel

    var body: some View {
        Card(title: "延迟", systemImage: "timer") {
            VStack(spacing: 8) {
                Picker("缓冲大小", selection: Binding(
                    get: { model.bufferFrames },
                    set: { model.setBufferFrames($0) }
                )) {
                    ForEach([16, 32, 64, 128, 256], id: \.self) { frames in
                        Text("\(frames) 帧").tag(frames)
                    }
                }
                .pickerStyle(.menu)

                HStack {
                    Text("实际缓冲")
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(model.latencyText)
                        .monospacedDigit()
                }
                .font(.caption)

                Picker("采样率", selection: Binding(
                    get: { model.sampleRate },
                    set: { model.setSampleRate($0) }
                )) {
                    Text("44.1 kHz").tag(44100.0)
                    Text("48 kHz").tag(48000.0)
                    Text("96 kHz").tag(96000.0)
                }
                .pickerStyle(.menu)

                HStack {
                    Text("实际采样率")
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(model.sampleRateText)
                        .monospacedDigit()
                }
                .font(.caption)
            }
        }
    }
}

struct ReverbCard: View {
    @ObservedObject var model: AudioModel

    var body: some View {
        Card(title: "混响", systemImage: "waveform") {
            VStack(spacing: 10) {
                Picker("预设", selection: Binding(
                    get: { model.reverbPreset },
                    set: { model.setReverbPreset($0) }
                )) {
                    ForEach(ReverbPreset.allCases) { preset in
                        Text(preset.title).tag(preset)
                    }
                }
                .pickerStyle(.menu)

                HStack(spacing: 10) {
                    Text("混响量")
                    Slider(value: Binding(
                        get: { model.reverbWet },
                        set: { model.setReverbWet($0) }
                    ), in: 0...0.6)
                    Text("\(Int((model.reverbWet * 100).rounded()))%")
                        .monospacedDigit()
                        .frame(width: 42, alignment: .trailing)
                }
                .disabled(model.reverbPreset == .off)
            }
        }
    }
}

struct LevelsCard: View {
    let meter: MeterModel

    var body: some View {
        Card(title: "电平", systemImage: "waveform.path.ecg") {
            LevelsContent(meter: meter)
        }
    }
}

private struct LevelsContent: View {
    @ObservedObject var meter: MeterModel

    var body: some View {
        VStack(spacing: 12) {
            MeterRow(title: "输入",
                     levelDB: meter.inputDB,
                     peakDB: meter.inputPeakDB)
            MeterRow(title: "输出",
                     levelDB: meter.outputDB,
                     peakDB: meter.outputPeakDB)

            HStack(spacing: 8) {
                Circle()
                    .fill(meter.clipping ? Color.red : Color.green.opacity(0.45))
                    .frame(width: 8, height: 8)
                Text(meter.clipping ? "削波" : "正常")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
                Text("过载 \(meter.overloadCount)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
    }
}

struct GainCard: View {
    @ObservedObject var model: AudioModel

    var body: some View {
        Card(title: "增益与音量", systemImage: "slider.horizontal.3") {
            VStack(spacing: 10) {
                HStack {
                    Text("输入增益")
                    Spacer()
                    Text(model.inputGainText)
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                Slider(value: Binding(
                    get: { model.inputGainDB },
                    set: { model.setInputGainDB($0) }
                ), in: -12...24)

                HStack {
                    Text("输出音量")
                    Spacer()
                    Text(model.outputVolumeText)
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                Slider(value: Binding(
                    get: { model.outputVolume },
                    set: { model.setOutputVolume($0) }
                ), in: 0...1)

                HStack {
                    Toggle("静音", isOn: Binding(
                        get: { model.isMuted },
                        set: { model.setMuted($0) }
                    ))
                    Spacer()
                    Text(model.muteShortcut)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}

struct CleanupCard: View {
    @ObservedObject var model: AudioModel

    var body: some View {
        Card(title: "清理", systemImage: "sparkles") {
            VStack(spacing: 8) {
                Toggle("高通滤波 80 Hz", isOn: Binding(
                    get: { model.highPassEnabled },
                    set: { model.setHighPassEnabled($0) }
                ))

                Toggle("自适应降噪", isOn: Binding(
                    get: { model.noiseGateEnabled },
                    set: { model.setNoiseGateEnabled($0) }
                ))

                HStack(spacing: 10) {
                    Text("门限")
                    Slider(value: Binding(
                        get: { model.gateThresholdDB },
                        set: { model.setGateThresholdDB($0) }
                    ), in: -60...(-20))
                    Text(model.gateThresholdText)
                        .monospacedDigit()
                        .frame(width: 52, alignment: .trailing)
                }
                .disabled(!model.noiseGateEnabled)

                Divider()

                Toggle("反馈抑制", isOn: Binding(
                    get: { model.feedbackSuppressionEnabled },
                    set: { model.setFeedbackSuppressionEnabled($0) }
                ))

                HStack {
                    Text("灵敏度")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Picker("", selection: Binding(
                        get: { model.feedbackSensitivity },
                        set: { model.setFeedbackSensitivity($0) }
                    )) {
                        Text("低").tag(0)
                        Text("中").tag(1)
                        Text("高").tag(2)
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                    .frame(width: 90)
                    Spacer()
                }
                .disabled(!model.feedbackSuppressionEnabled)
                .opacity(model.feedbackSuppressionEnabled ? 1 : 0.55)
            }
        }
    }
}

struct ToneCard: View {
    @ObservedObject var model: AudioModel

    var body: some View {
        Card(title: "音色", systemImage: "slider.vertical.3") {
            VStack(spacing: 8) {
                Toggle("3 段 EQ", isOn: Binding(
                    get: { model.eqEnabled },
                    set: { model.setEQEnabled($0) }
                ))

                VStack(spacing: 8) {
                    eqRow("低频 120 Hz", value: Binding(
                        get: { model.eqLowDB },
                        set: { model.setEQLowDB($0) }
                    ))
                    eqRow("中频 1 kHz", value: Binding(
                        get: { model.eqMidDB },
                        set: { model.setEQMidDB($0) }
                    ))
                    eqRow("高频 6 kHz", value: Binding(
                        get: { model.eqHighDB },
                        set: { model.setEQHighDB($0) }
                    ))
                }
                .disabled(!model.eqEnabled)
                .opacity(model.eqEnabled ? 1 : 0.55)

                Divider()

                Toggle("压缩", isOn: Binding(
                    get: { model.compressorEnabled },
                    set: { model.setCompressorEnabled($0) }
                ))

                HStack(spacing: 8) {
                    Text("阈值")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Slider(value: Binding(
                        get: { model.compThresholdDB },
                        set: { model.setCompThresholdDB($0) }
                    ), in: -40...(-6))
                    Text("\(Int(model.compThresholdDB.rounded())) dB")
                        .font(.caption.monospacedDigit())
                        .frame(width: 52, alignment: .trailing)
                }
                .disabled(!model.compressorEnabled)
                .opacity(model.compressorEnabled ? 1 : 0.55)

                HStack(spacing: 8) {
                    Text("比率")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Slider(value: Binding(
                        get: { model.compRatio },
                        set: { model.setCompRatio($0) }
                    ), in: 1...10)
                    Text(String(format: "%.1f:1", model.compRatio))
                        .font(.caption.monospacedDigit())
                        .frame(width: 52, alignment: .trailing)
                }
                .disabled(!model.compressorEnabled)
                .opacity(model.compressorEnabled ? 1 : 0.55)

                HStack {
                    Image(systemName: "shield.lefthalf.filled")
                        .font(.caption2)
                    Text("限幅保护 · 始终开启")
                        .font(.caption2)
                    Spacer()
                }
                .foregroundStyle(.secondary)
            }
        }
    }

    private func eqRow(_ title: String, value: Binding<Double>) -> some View {
        HStack(spacing: 8) {
            Text(title)
                .font(.caption)
                .foregroundStyle(.secondary)
            Slider(value: value, in: -12...12)
            Text(String(format: "%+.1f dB", value.wrappedValue))
                .font(.caption.monospacedDigit())
                .frame(width: 58, alignment: .trailing)
        }
    }
}

struct GeneralCard: View {
    @ObservedObject var model: AudioModel

    var body: some View {
        Card(title: "启动", systemImage: "power") {
            VStack(spacing: 8) {
                Toggle("登录时自动启动", isOn: Binding(
                    get: { model.launchAtLogin },
                    set: { model.setLaunchAtLogin($0) }
                ))

                HStack {
                    Text("静音快捷键")
                        .foregroundStyle(.secondary)
                    Spacer()
                    Text(model.muteShortcut)
                        .monospaced()
                }
                .font(.caption)
            }
        }
    }
}

struct DiagnosticsCard: View {
    let model: AudioModel
    let meter: MeterModel

    var body: some View {
        Card(title: "诊断", systemImage: "gauge.with.dots.needle.33percent") {
            DiagnosticsContent(model: model, meter: meter)
        }
    }
}

private struct DiagnosticsContent: View {
    @ObservedObject var model: AudioModel
    @ObservedObject var meter: MeterModel

    var body: some View {
        VStack(spacing: 6) {
            diagnosticRow("过载", "\(meter.overloadCount)")
            diagnosticRow("削波样本", "\(meter.clipCount)")
            diagnosticRow("反馈陷波", "\(meter.feedbackNotches)")
            diagnosticRow("回调", "\(meter.callbackCount)")
            diagnosticRow("缓冲", "\(model.actualBufferFrames) 帧")

            Button {
                model.resetDiagnostics()
            } label: {
                Label("重置统计", systemImage: "arrow.counterclockwise")
            }
            .modifier(GlassRegularButtonModifier())
            .padding(.top, 4)
        }
        .font(.caption)
    }

    private func diagnosticRow(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title).foregroundStyle(.secondary)
            Spacer()
            Text(value).monospacedDigit()
        }
    }
}

// MARK: - Main window

struct ContentView: View {
    @EnvironmentObject private var model: AudioModel

    var body: some View {
        ZStack {
            background

            VStack(spacing: 0) {
                ScrollView {
                    VStack(spacing: 18) {
                        header

                        HStack(alignment: .top, spacing: 16) {
                            VStack(spacing: 16) {
                                PresetCard(model: model)
                                DevicesCard(model: model)
                                BufferCard(model: model)
                                ReverbCard(model: model)
                                DiagnosticsCard(model: model, meter: model.meter)
                            }
                            .frame(maxWidth: .infinity, alignment: .top)

                            VStack(spacing: 16) {
                                LevelsCard(meter: model.meter)
                                GainCard(model: model)
                                CleanupCard(model: model)
                                ToneCard(model: model)
                                GeneralCard(model: model)
                            }
                            .frame(maxWidth: .infinity, alignment: .top)
                        }
                    }
                    .padding(22)
                }

                bottomBar
            }
        }
        .frame(width: 820, height: 760)
        .alert("音频错误", isPresented: errorBinding) {
            Button("好") {
                model.errorMessage = nil
            }
        } message: {
            Text(model.errorMessage ?? "")
        }
    }

    private var background: some View {
        ZStack {
            Color(nsColor: .windowBackgroundColor)

            LinearGradient(
                colors: [
                    Color.accentColor.opacity(0.12),
                    Color.clear,
                    Color.blue.opacity(0.05)
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )

            Circle()
                .fill(Color.blue.opacity(0.10))
                .frame(width: 420, height: 420)
                .blur(radius: 90)
                .offset(x: -280, y: -260)

            Circle()
                .fill(Color.purple.opacity(0.08))
                .frame(width: 380, height: 380)
                .blur(radius: 90)
                .offset(x: 320, y: 280)
        }
        .ignoresSafeArea()
    }

    private var header: some View {
        HStack(spacing: 14) {
            ZStack {
                Circle()
                    .fill(.tint.opacity(0.16))
                    .frame(width: 60, height: 60)
                Image(systemName: "mic.fill")
                    .font(.system(size: 28, weight: .semibold))
                    .foregroundStyle(.tint)
            }

            VStack(alignment: .leading, spacing: 3) {
                Text("EarCue")
                    .font(.system(size: 30, weight: .bold, design: .rounded))
                Text(model.isRunning ? "正在监听麦克风" : "耳返 · 原生低延迟音频监听")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            HStack(spacing: 6) {
                Circle()
                    .fill(model.isRunning ? Color.green : Color.secondary.opacity(0.4))
                    .frame(width: 8, height: 8)
                Text(model.isRunning ? "监听中" : "未启动")
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
            .modifier(GlassPill())
        }
    }

    private var bottomBar: some View {
        HStack(spacing: 16) {
            HStack(spacing: 8) {
                Circle()
                    .fill(model.isRunning ? Color.green : Color.secondary.opacity(0.4))
                    .frame(width: 8, height: 8)
                Text(model.isRunning ? "监听中" : "未启动")
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Button {
                model.toggleMonitoring()
            } label: {
                Label(model.isRunning ? "停止监听" : "开始监听",
                      systemImage: model.isRunning ? "stop.fill" : "play.fill")
                    .frame(minWidth: 160)
            }
            .modifier(GlassProminentButtonModifier(tint: model.isRunning ? .red : .green))
            .controlSize(.large)

            Button("退出") {
                NSApplication.shared.terminate(nil)
            }
            .modifier(GlassRegularButtonModifier())
        }
        .padding(.horizontal, 22)
        .padding(.vertical, 14)
        .background(.ultraThinMaterial)
        .overlay(alignment: .top) {
            Rectangle()
                .fill(.white.opacity(0.08))
                .frame(height: 1)
        }
    }

    private var errorBinding: Binding<Bool> {
        Binding(
            get: { model.errorMessage != nil },
            set: { newValue in
                if !newValue { model.errorMessage = nil }
            }
        )
    }
}
