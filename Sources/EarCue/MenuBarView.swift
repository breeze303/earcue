import SwiftUI
import AppKit

struct MenuBarPanelView: View {
    @EnvironmentObject private var model: AudioModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 8) {
                Image(systemName: "mic.fill")
                    .foregroundStyle(.tint)
                Text("EarCue")
                    .font(.headline)
                Spacer()
                HStack(spacing: 5) {
                    Circle()
                        .fill(model.isRunning ? Color.green : Color.secondary.opacity(0.4))
                        .frame(width: 7, height: 7)
                    Text(model.isRunning ? "监听中" : "未启动")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Button {
                model.toggleMonitoring()
            } label: {
                Label(model.isRunning ? "停止监听" : "开始监听",
                      systemImage: model.isRunning ? "stop.fill" : "play.fill")
                    .frame(maxWidth: .infinity)
            }
            .modifier(GlassProminentButtonModifier(tint: model.isRunning ? .red : .green))
            .controlSize(.large)

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

            HStack(spacing: 8) {
                Text("音量")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Slider(value: Binding(
                    get: { model.outputVolume },
                    set: { model.setOutputVolume($0) }
                ), in: 0...1)
                Text(model.outputVolumeText)
                    .font(.caption.monospacedDigit())
                    .frame(width: 36, alignment: .trailing)
            }

            Divider()

            Menu("输入：\(model.selectedInput?.name ?? "未选择")") {
                ForEach(model.inputDevices) { device in
                    Button {
                        model.selectInput(device.id)
                    } label: {
                        if device.id == model.selectedInputID {
                            Label(device.name, systemImage: "checkmark")
                        } else {
                            Text(device.name)
                        }
                    }
                }
            }

            Menu("输出：\(model.selectedOutput?.name ?? "未选择")") {
                ForEach(model.outputDevices) { device in
                    Button {
                        model.selectOutput(device.id)
                    } label: {
                        if device.id == model.selectedOutputID {
                            Label(device.name, systemImage: "checkmark")
                        } else {
                            Text(device.name)
                        }
                    }
                }
            }

            Menu("混响：\(model.reverbPreset.title)") {
                ForEach(ReverbPreset.allCases) { preset in
                    Button {
                        model.setReverbPreset(preset)
                    } label: {
                        if preset == model.reverbPreset {
                            Label(preset.title, systemImage: "checkmark")
                        } else {
                            Text(preset.title)
                        }
                    }
                }
            }

            Menu("场景：\(model.preset.title)") {
                ForEach(MonitorPreset.allCases) { preset in
                    Button {
                        model.applyPreset(preset)
                    } label: {
                        if preset == model.preset {
                            Label(preset.title, systemImage: "checkmark")
                        } else {
                            Text(preset.title)
                        }
                    }
                }
            }

            Divider()

            HStack {
                Button("打开主窗口") {
                    NSApp.activate(ignoringOtherApps: true)
                    openWindow(id: "main")
                }
                .modifier(GlassRegularButtonModifier())

                Spacer()

                Button("退出") {
                    NSApplication.shared.terminate(nil)
                }
                .modifier(GlassRegularButtonModifier())
            }
        }
        .padding(16)
        .frame(width: 320)
    }
}
