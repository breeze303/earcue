import Foundation
import Combine
import AppKit
import ServiceManagement
import EarCueAudio

enum MonitorPreset: String, CaseIterable, Identifiable {
    case podcast
    case singing
    case gaming
    case meeting
    case custom

    var id: String { rawValue }

    var title: String {
        switch self {
        case .podcast: return "播客"
        case .singing: return "唱歌"
        case .gaming: return "游戏"
        case .meeting: return "会议"
        case .custom: return "自定义"
        }
    }

    var detail: String {
        switch self {
        case .podcast: return "增益 +6 dB，高通 + 噪声门，干声"
        case .singing: return "32 帧，厅堂混响，高通"
        case .gaming: return "32 帧，干声，零处理"
        case .meeting: return "64 帧，高通 + 噪声门"
        case .custom: return "使用当前手动设置"
        }
    }
}

enum ReverbPreset: Int, CaseIterable, Identifiable {
    case off = 0
    case room = 1
    case hall = 2
    case large = 3

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .off: return "关闭"
        case .room: return "房间"
        case .hall: return "厅堂"
        case .large: return "大空间"
        }
    }
}

@MainActor
final class MeterModel: ObservableObject {
    // Display values are smoothed in dBFS. Fast attack, slow release gives
    // a stable meter without hiding transients or clipping.
    @Published var inputDB: Double = -60
    @Published var outputDB: Double = -60
    @Published var inputPeakDB: Double = -60
    @Published var outputPeakDB: Double = -60
    @Published var clipping = false
    @Published var clipCount = 0
    @Published var overloadCount = 0
    @Published var callbackCount: UInt64 = 0
    @Published var feedbackNotches = 0

    private var lastUpdate = Date.distantPast
    private var inputPeakUntil = Date.distantPast
    private var outputPeakUntil = Date.distantPast
    private var clipUntil = Date.distantPast
    private var lastClipCount = 0

    func update(inputRMS: Float,
                inputPeak: Float,
                outputRMS: Float,
                outputPeak: Float,
                clipCount: Int,
                overloadCount: Int,
                callbackCount: UInt64,
                feedbackNotches: Int) {
        let now = Date()
        let dt = min(max(now.timeIntervalSince(lastUpdate), 0.01), 0.5)
        lastUpdate = now

        let attack = 1.0 - exp(-dt / 0.015)
        let release = 1.0 - exp(-dt / 0.35)
        inputDB = smooth(inputDB, Self.db(inputRMS), attack: attack, release: release)
        outputDB = smooth(outputDB, Self.db(outputRMS), attack: attack, release: release)

        let inputPeakTarget = Self.db(inputPeak)
        if inputPeakTarget >= inputPeakDB {
            inputPeakDB = inputPeakTarget
            inputPeakUntil = now.addingTimeInterval(1.2)
        } else if now > inputPeakUntil {
            inputPeakDB = max(inputDB, inputPeakDB - dt * 24.0)
        }

        let outputPeakTarget = Self.db(outputPeak)
        if outputPeakTarget >= outputPeakDB {
            outputPeakDB = outputPeakTarget
            outputPeakUntil = now.addingTimeInterval(1.2)
        } else if now > outputPeakUntil {
            outputPeakDB = max(outputDB, outputPeakDB - dt * 24.0)
        }

        if clipCount > lastClipCount {
            clipping = true
            clipUntil = now.addingTimeInterval(1.5)
        }
        self.clipCount = clipCount
        lastClipCount = clipCount
        if now > clipUntil {
            clipping = false
        }
        self.overloadCount = overloadCount
        self.callbackCount = callbackCount
        self.feedbackNotches = feedbackNotches
    }

    private func smooth(_ current: Double, _ target: Double,
                        attack: Double, release: Double) -> Double {
        let coefficient = target > current ? attack : release
        return current + (target - current) * coefficient
    }

    private static func db(_ value: Float) -> Double {
        let v = max(Double(value), 1e-6)
        return min(0.0, max(-60.0, 20.0 * log10(v)))
    }

    func reset() {
        inputDB = -60
        outputDB = -60
        inputPeakDB = -60
        outputPeakDB = -60
        clipping = false
        clipCount = 0
        overloadCount = 0
        callbackCount = 0
        feedbackNotches = 0
        lastClipCount = 0
        lastUpdate = .distantPast
        inputPeakUntil = .distantPast
        outputPeakUntil = .distantPast
        clipUntil = .distantPast
    }
}

@MainActor
final class AudioModel: ObservableObject {
    @Published private(set) var devices: [AudioDevice] = []
    @Published private(set) var selectedInputID: UInt32 = 0
    @Published private(set) var selectedOutputID: UInt32 = 0
    @Published private(set) var bufferFrames: Int = 32
    @Published private(set) var reverbPreset: ReverbPreset = .off
    @Published private(set) var reverbWet: Double = 0.24
    @Published private(set) var isRunning = false
    @Published private(set) var preset: MonitorPreset = .custom
    @Published private(set) var launchAtLogin = false
    @Published private(set) var sampleRate: Double = 48000

    @Published private(set) var inputGainDB: Double = 0
    @Published private(set) var outputVolume: Double = 1
    @Published private(set) var isMuted = false
    @Published private(set) var highPassEnabled = false
    @Published private(set) var noiseGateEnabled = false
    @Published private(set) var gateThresholdDB: Double = -50

    @Published private(set) var eqEnabled = false
    @Published private(set) var eqLowDB: Double = 0
    @Published private(set) var eqMidDB: Double = 0
    @Published private(set) var eqHighDB: Double = 0
    @Published private(set) var compressorEnabled = false
    @Published private(set) var compThresholdDB: Double = -24
    @Published private(set) var compRatio: Double = 3
    @Published private(set) var feedbackSuppressionEnabled = false
    @Published private(set) var feedbackSensitivity = 1

    @Published var errorMessage: String?

    let meter = MeterModel()
    let muteShortcut = "⌃⌥M"

    private var engine: OpaquePointer?
    private var meterTimer: Timer?
    private var deviceTimer: Timer?
    private let deviceMonitor = DeviceMonitor()
    private let defaults = UserDefaults.standard

    private struct DeviceDTO: Decodable {
        let id: UInt32
        let name: String
        let uid: String
        let input: Int
        let output: Int
        let sampleRate: Double
    }

    init() {
        bufferFrames = defaults.object(forKey: "bufferFrames") as? Int ?? 32
        reverbPreset = ReverbPreset(rawValue: defaults.integer(forKey: "reverbPreset")) ?? .off
        reverbWet = defaults.object(forKey: "reverbWet") as? Double ?? 0.24
        preset = MonitorPreset(rawValue: defaults.string(forKey: "preset") ?? "") ?? .custom
        launchAtLogin = SMAppService.mainApp.status == .enabled
        sampleRate = defaults.object(forKey: "sampleRate") as? Double ?? 48000

        inputGainDB = defaults.object(forKey: "inputGainDB") as? Double ?? 0
        outputVolume = defaults.object(forKey: "outputVolume") as? Double ?? 1
        isMuted = defaults.bool(forKey: "isMuted")
        highPassEnabled = defaults.bool(forKey: "highPassEnabled")
        noiseGateEnabled = defaults.bool(forKey: "noiseGateEnabled")
        gateThresholdDB = defaults.object(forKey: "gateThresholdDB") as? Double ?? -50

        eqEnabled = defaults.bool(forKey: "eqEnabled")
        eqLowDB = defaults.object(forKey: "eqLowDB") as? Double ?? 0
        eqMidDB = defaults.object(forKey: "eqMidDB") as? Double ?? 0
        eqHighDB = defaults.object(forKey: "eqHighDB") as? Double ?? 0
        compressorEnabled = defaults.bool(forKey: "compressorEnabled")
        compThresholdDB = defaults.object(forKey: "compThresholdDB") as? Double ?? -24
        compRatio = defaults.object(forKey: "compRatio") as? Double ?? 3
        feedbackSuppressionEnabled = defaults.bool(forKey: "feedbackSuppressionEnabled")
        feedbackSensitivity = defaults.object(forKey: "feedbackSensitivity") as? Int ?? 1

        refreshDevices()

        deviceMonitor.onChange = { [weak self] in
            Task { @MainActor in
                self?.refreshDevices()
            }
        }
        if !deviceMonitor.start() {
            // Fallback for unusual Core Audio configurations: poll slowly.
            deviceTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
                Task { @MainActor in
                    self?.refreshDevices()
                }
            }
        }

        HotKeyManager.shared.onToggleMute = { [weak self] in
            Task { @MainActor in
                self?.toggleMute()
            }
        }
        HotKeyManager.shared.register()
    }

    var inputDevices: [AudioDevice] {
        devices.filter { $0.isInput }
    }

    var outputDevices: [AudioDevice] {
        devices.filter { $0.isOutput }
    }

    var selectedInput: AudioDevice? {
        devices.first { $0.id == selectedInputID && $0.isInput }
    }

    var selectedOutput: AudioDevice? {
        devices.first { $0.id == selectedOutputID && $0.isOutput }
    }

    var inputGainText: String {
        let value = abs(inputGainDB) < 0.05 ? 0.0 : inputGainDB
        return String(format: "%+.1f dB", value)
    }

    var outputVolumeText: String {
        "\(Int((outputVolume * 100).rounded()))%"
    }

    var gateThresholdText: String {
        "\(Int(gateThresholdDB.rounded())) dB"
    }

    var sampleRateText: String {
        let rate = engine.map { ha_engine_sample_rate($0) } ?? sampleRate
        return "\(Int(rate.rounded())) Hz"
    }

    var presetDescription: String {
        preset.detail
    }

    var actualBufferFrames: Int {
        engine.map { Int(ha_engine_latency_frames($0)) } ?? bufferFrames
    }

    var latencyText: String {
        guard let engine else { return "未启动" }
        let frames = ha_engine_latency_frames(engine)
        let rate = ha_engine_sample_rate(engine)
        guard rate > 0 else { return "\(frames) 帧" }
        let milliseconds = Double(frames) / rate * 1000.0
        return String(format: "%d 帧 · %.2f ms 缓冲", frames, milliseconds)
    }

    func refreshDevices() {
        var buffer = [CChar](repeating: 0, count: 65536)
        let length = buffer.withUnsafeMutableBufferPointer { pointer in
            ha_list_devices_json(pointer.baseAddress, Int32(pointer.count))
        }
        guard length > 0 else { return }

        let data = buffer.withUnsafeBytes { rawBuffer in
            Data(rawBuffer.prefix(Int(length)))
        }
        guard let decoded = try? JSONDecoder().decode([DeviceDTO].self, from: data) else {
            return
        }

        if isRunning {
            let inputStillExists = decoded.contains { $0.id == selectedInputID && $0.input > 0 }
            let outputStillExists = decoded.contains { $0.id == selectedOutputID && $0.output > 0 }
            if !inputStillExists || !outputStillExists {
                stop()
                errorMessage = "所选音频设备已断开。"
            }
        }

        let newDevices = decoded.map {
            AudioDevice(
                id: $0.id,
                name: $0.name,
                uid: $0.uid,
                inputChannels: $0.input,
                outputChannels: $0.output,
                sampleRate: $0.sampleRate
            )
        }
        if newDevices != devices {
            devices = newDevices
        }

        restoreSelectionIfNeeded()
    }

    private func restoreSelectionIfNeeded() {
        if !isRunning {
            if let savedUID = defaults.string(forKey: "inputUID"),
               let saved = inputDevices.first(where: { $0.uid == savedUID }) {
                selectedInputID = saved.id
            } else if !inputDevices.contains(where: { $0.id == selectedInputID }) {
                let defaultID = ha_default_input_device()
                selectedInputID = inputDevices.first(where: { $0.id == defaultID })?.id
                    ?? inputDevices.first?.id
                    ?? 0
            }

            if let savedUID = defaults.string(forKey: "outputUID"),
               let saved = outputDevices.first(where: { $0.uid == savedUID }) {
                selectedOutputID = saved.id
            } else if !outputDevices.contains(where: { $0.id == selectedOutputID }) {
                let defaultID = ha_default_output_device()
                selectedOutputID = outputDevices.first(where: { $0.id == defaultID })?.id
                    ?? outputDevices.first?.id
                    ?? 0
            }
        }
    }

    func selectInput(_ id: UInt32) {
        guard id != selectedInputID else { return }
        selectedInputID = id
        if let device = inputDevices.first(where: { $0.id == id }) {
            defaults.set(device.uid, forKey: "inputUID")
        }
        restartIfRunning()
    }

    func selectOutput(_ id: UInt32) {
        guard id != selectedOutputID else { return }
        selectedOutputID = id
        if let device = outputDevices.first(where: { $0.id == id }) {
            defaults.set(device.uid, forKey: "outputUID")
        }
        restartIfRunning()
    }

    func setBufferFrames(_ frames: Int) {
        guard frames != bufferFrames, frames > 0 else { return }
        bufferFrames = frames
        defaults.set(frames, forKey: "bufferFrames")
        markCustom()
        restartIfRunning()
    }

    func setReverbPreset(_ preset: ReverbPreset) {
        guard preset != reverbPreset else { return }
        reverbPreset = preset
        defaults.set(preset.rawValue, forKey: "reverbPreset")
        markCustom()
        applyReverb()
    }

    func setReverbWet(_ wet: Double) {
        let clamped = min(max(wet, 0.0), 0.6)
        guard abs(clamped - reverbWet) > 0.0001 else { return }
        reverbWet = clamped
        defaults.set(clamped, forKey: "reverbWet")
        markCustom()
        applyReverb()
    }

    func setInputGainDB(_ value: Double) {
        let clamped = min(max(value, -12), 24)
        guard abs(clamped - inputGainDB) > 0.01 else { return }
        inputGainDB = clamped
        defaults.set(clamped, forKey: "inputGainDB")
        markCustom()
        applyGain()
    }

    func setOutputVolume(_ value: Double) {
        let clamped = min(max(value, 0), 1)
        guard abs(clamped - outputVolume) > 0.001 else { return }
        outputVolume = clamped
        defaults.set(clamped, forKey: "outputVolume")
        markCustom()
        applyGain()
    }

    func toggleMute() {
        setMuted(!isMuted)
    }

    func setMuted(_ muted: Bool) {
        guard muted != isMuted else { return }
        isMuted = muted
        defaults.set(muted, forKey: "isMuted")
        applyGain()
    }

    func setHighPassEnabled(_ enabled: Bool) {
        guard enabled != highPassEnabled else { return }
        highPassEnabled = enabled
        defaults.set(enabled, forKey: "highPassEnabled")
        markCustom()
        applyCleanup()
    }

    func setNoiseGateEnabled(_ enabled: Bool) {
        guard enabled != noiseGateEnabled else { return }
        noiseGateEnabled = enabled
        defaults.set(enabled, forKey: "noiseGateEnabled")
        markCustom()
        applyCleanup()
    }

    func setGateThresholdDB(_ value: Double) {
        let clamped = min(max(value, -60), -20)
        guard abs(clamped - gateThresholdDB) > 0.1 else { return }
        gateThresholdDB = clamped
        defaults.set(clamped, forKey: "gateThresholdDB")
        markCustom()
        applyCleanup()
    }

    func setEQEnabled(_ enabled: Bool) {
        guard enabled != eqEnabled else { return }
        eqEnabled = enabled
        defaults.set(enabled, forKey: "eqEnabled")
        markCustom()
        applyTone()
    }

    func setEQLowDB(_ value: Double) {
        let clamped = min(max(value, -12), 12)
        guard abs(clamped - eqLowDB) > 0.01 else { return }
        eqLowDB = clamped
        defaults.set(clamped, forKey: "eqLowDB")
        markCustom()
        applyTone()
    }

    func setEQMidDB(_ value: Double) {
        let clamped = min(max(value, -12), 12)
        guard abs(clamped - eqMidDB) > 0.01 else { return }
        eqMidDB = clamped
        defaults.set(clamped, forKey: "eqMidDB")
        markCustom()
        applyTone()
    }

    func setEQHighDB(_ value: Double) {
        let clamped = min(max(value, -12), 12)
        guard abs(clamped - eqHighDB) > 0.01 else { return }
        eqHighDB = clamped
        defaults.set(clamped, forKey: "eqHighDB")
        markCustom()
        applyTone()
    }

    func setCompressorEnabled(_ enabled: Bool) {
        guard enabled != compressorEnabled else { return }
        compressorEnabled = enabled
        defaults.set(enabled, forKey: "compressorEnabled")
        markCustom()
        applyTone()
    }

    func setCompThresholdDB(_ value: Double) {
        let clamped = min(max(value, -40), -6)
        guard abs(clamped - compThresholdDB) > 0.1 else { return }
        compThresholdDB = clamped
        defaults.set(clamped, forKey: "compThresholdDB")
        markCustom()
        applyTone()
    }

    func setCompRatio(_ value: Double) {
        let clamped = min(max(value, 1), 10)
        guard abs(clamped - compRatio) > 0.01 else { return }
        compRatio = clamped
        defaults.set(clamped, forKey: "compRatio")
        markCustom()
        applyTone()
    }

    func setFeedbackSuppressionEnabled(_ enabled: Bool) {
        guard enabled != feedbackSuppressionEnabled else { return }
        feedbackSuppressionEnabled = enabled
        defaults.set(enabled, forKey: "feedbackSuppressionEnabled")
        applyFeedback()
    }

    func setFeedbackSensitivity(_ value: Int) {
        let clamped = min(max(value, 0), 2)
        guard clamped != feedbackSensitivity else { return }
        feedbackSensitivity = clamped
        defaults.set(clamped, forKey: "feedbackSensitivity")
        applyFeedback()
    }

    func setSampleRate(_ rate: Double) {
        let allowed: [Double] = [44100, 48000, 96000]
        guard allowed.contains(rate), rate != sampleRate else { return }
        sampleRate = rate
        defaults.set(rate, forKey: "sampleRate")
        restartIfRunning()
    }

    func applyPreset(_ newPreset: MonitorPreset) {
        switch newPreset {
        case .podcast:
            bufferFrames = 64
            inputGainDB = 6
            outputVolume = 1
            highPassEnabled = true
            noiseGateEnabled = true
            gateThresholdDB = -45
            reverbPreset = .off
            reverbWet = 0.24
            eqEnabled = true
            eqLowDB = -2
            eqMidDB = 1
            eqHighDB = 1
            compressorEnabled = true
            compThresholdDB = -24
            compRatio = 3
        case .singing:
            bufferFrames = 32
            inputGainDB = 3
            outputVolume = 1
            highPassEnabled = true
            noiseGateEnabled = false
            gateThresholdDB = -50
            reverbPreset = .hall
            reverbWet = 0.25
            eqEnabled = true
            eqLowDB = -1
            eqMidDB = 1
            eqHighDB = 2
            compressorEnabled = true
            compThresholdDB = -22
            compRatio = 2.5
        case .gaming:
            bufferFrames = 32
            inputGainDB = 0
            outputVolume = 1
            highPassEnabled = false
            noiseGateEnabled = false
            gateThresholdDB = -50
            reverbPreset = .off
            reverbWet = 0.24
            eqEnabled = false
            eqLowDB = 0
            eqMidDB = 0
            eqHighDB = 0
            compressorEnabled = false
            compThresholdDB = -24
            compRatio = 3
        case .meeting:
            bufferFrames = 64
            inputGainDB = 3
            outputVolume = 1
            highPassEnabled = true
            noiseGateEnabled = true
            gateThresholdDB = -40
            reverbPreset = .off
            reverbWet = 0.24
            eqEnabled = true
            eqLowDB = -3
            eqMidDB = 2
            eqHighDB = 1
            compressorEnabled = true
            compThresholdDB = -26
            compRatio = 3
        case .custom:
            preset = .custom
            persistSettings()
            return
        }

        preset = newPreset
        persistSettings()
        if isRunning {
            stop()
            start()
        }
    }

    func setLaunchAtLogin(_ enabled: Bool) {
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            launchAtLogin = SMAppService.mainApp.status == .enabled
        } catch {
            launchAtLogin = SMAppService.mainApp.status == .enabled
            errorMessage = "无法设置开机自启：\(error.localizedDescription)"
        }
    }

    private func markCustom() {
        guard preset != .custom else { return }
        preset = .custom
        defaults.set(MonitorPreset.custom.rawValue, forKey: "preset")
    }

    private func persistSettings() {
        defaults.set(bufferFrames, forKey: "bufferFrames")
        defaults.set(reverbPreset.rawValue, forKey: "reverbPreset")
        defaults.set(reverbWet, forKey: "reverbWet")
        defaults.set(inputGainDB, forKey: "inputGainDB")
        defaults.set(outputVolume, forKey: "outputVolume")
        defaults.set(highPassEnabled, forKey: "highPassEnabled")
        defaults.set(noiseGateEnabled, forKey: "noiseGateEnabled")
        defaults.set(gateThresholdDB, forKey: "gateThresholdDB")
        defaults.set(eqEnabled, forKey: "eqEnabled")
        defaults.set(eqLowDB, forKey: "eqLowDB")
        defaults.set(eqMidDB, forKey: "eqMidDB")
        defaults.set(eqHighDB, forKey: "eqHighDB")
        defaults.set(compressorEnabled, forKey: "compressorEnabled")
        defaults.set(compThresholdDB, forKey: "compThresholdDB")
        defaults.set(compRatio, forKey: "compRatio")
        defaults.set(feedbackSuppressionEnabled, forKey: "feedbackSuppressionEnabled")
        defaults.set(feedbackSensitivity, forKey: "feedbackSensitivity")
        defaults.set(preset.rawValue, forKey: "preset")
    }

    func start() {
        guard !isRunning else { return }
        guard let input = selectedInput else {
            errorMessage = "请选择输入设备。"
            return
        }
        guard let output = selectedOutput else {
            errorMessage = "请选择输出设备。"
            return
        }

        let newEngine = ha_engine_create(
            input.id,
            output.id,
            Int32(bufferFrames),
            sampleRate
        )
        guard let newEngine else {
            errorMessage = "无法创建音频引擎。"
            return
        }

        let createError = String(cString: ha_engine_error(newEngine))
        if !createError.isEmpty {
            ha_engine_destroy(newEngine)
            errorMessage = "无法创建音频引擎：\(createError)"
            return
        }

        ha_engine_set_reverb(newEngine, Int32(reverbPreset.rawValue), Float(reverbWet))
        applyGain(to: newEngine)
        applyCleanup(to: newEngine)
        applyTone(to: newEngine)
        applyFeedback(to: newEngine)

        if ha_engine_start(newEngine) != 0 {
            let startError = String(cString: ha_engine_error(newEngine))
            ha_engine_destroy(newEngine)
            errorMessage = startError.isEmpty
                ? "无法启动音频。"
                : "无法启动音频：\(startError)"
            return
        }

        engine = newEngine
        isRunning = true
        startMeter()
    }

    func stop() {
        if let engine {
            ha_engine_stop(engine)
            ha_engine_destroy(engine)
        }
        engine = nil
        isRunning = false
        meter.reset()
        meterTimer?.invalidate()
        meterTimer = nil
    }

    func toggleMonitoring() {
        if isRunning {
            stop()
        } else {
            start()
        }
    }

    func resetDiagnostics() {
        if let engine {
            ha_engine_reset_clip_count(engine)
            ha_engine_reset_overload_count(engine)
        }
        meter.reset()
    }

    private func restartIfRunning() {
        guard isRunning else { return }
        stop()
        start()
    }

    private func applyReverb() {
        guard let engine else { return }
        ha_engine_set_reverb(engine, Int32(reverbPreset.rawValue), Float(reverbWet))
    }

    private func applyGain() {
        guard let engine else { return }
        applyGain(to: engine)
    }

    private func applyGain(to engine: OpaquePointer) {
        let inputGain = Float(pow(10.0, inputGainDB / 20.0))
        ha_engine_set_gain(engine, inputGain, Float(outputVolume), isMuted ? 1 : 0)
    }

    private func applyCleanup() {
        guard let engine else { return }
        applyCleanup(to: engine)
    }

    private func applyCleanup(to engine: OpaquePointer) {
        ha_engine_set_cleanup(
            engine,
            highPassEnabled ? 1 : 0,
            noiseGateEnabled ? 1 : 0,
            Float(gateThresholdDB)
        )
    }

    private func applyTone() {
        guard let engine else { return }
        applyTone(to: engine)
    }

    private func applyTone(to engine: OpaquePointer) {
        ha_engine_set_eq(
            engine,
            eqEnabled ? 1 : 0,
            Float(eqLowDB),
            Float(eqMidDB),
            Float(eqHighDB)
        )
        ha_engine_set_compressor(
            engine,
            compressorEnabled ? 1 : 0,
            Float(compThresholdDB),
            Float(compRatio)
        )
    }

    private func applyFeedback() {
        guard let engine else { return }
        applyFeedback(to: engine)
    }

    private func applyFeedback(to engine: OpaquePointer) {
        ha_engine_set_feedback_suppression(
            engine,
            feedbackSuppressionEnabled ? 1 : 0,
            Int32(feedbackSensitivity)
        )
    }

    private func startMeter() {
        meterTimer?.invalidate()
        meterTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 10.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self, let engine = self.engine else { return }
                // The meters are only useful while the app is frontmost.
                // Skipping redraws in the background keeps a menu-bar
                // monitor close to 0% CPU while another app is focused.
                guard NSApp.isActive else { return }
                self.meter.update(
                    inputRMS: ha_engine_input_level(engine),
                    inputPeak: ha_engine_input_peak(engine),
                    outputRMS: ha_engine_output_level(engine),
                    outputPeak: ha_engine_output_peak(engine),
                    clipCount: Int(ha_engine_clip_count(engine)),
                    overloadCount: Int(ha_engine_overload_count(engine)),
                    callbackCount: ha_engine_callback_count(engine),
                    feedbackNotches: Int(ha_engine_feedback_notch_count(engine))
                )
            }
        }
    }
}
