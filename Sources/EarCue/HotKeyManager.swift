import Carbon.HIToolbox
import Foundation

/// Registers a system-wide hotkey without requiring Accessibility access.
/// The hotkey is Control-Option-M and toggles the monitor mute.
final class HotKeyManager {
    static let shared = HotKeyManager()

    var onToggleMute: (() -> Void)?

    private var hotKeyRef: EventHotKeyRef?
    private var handlerRef: EventHandlerRef?
    private var installed = false

    private init() {}

    func register() {
        guard !installed else { return }

        var eventType = EventTypeSpec(
            eventClass: OSType(kEventClassKeyboard),
            eventKind: UInt32(kEventHotKeyPressed)
        )

        let handlerStatus = InstallEventHandler(
            GetApplicationEventTarget(),
            { _, event, _ in
                guard let event else { return noErr }
                var hotKeyID = EventHotKeyID()
                let status = withUnsafeMutablePointer(to: &hotKeyID) { pointer in
                    GetEventParameter(
                        event,
                        EventParamName(kEventParamDirectObject),
                        EventParamType(typeEventHotKeyID),
                        nil,
                        MemoryLayout<EventHotKeyID>.size,
                        nil,
                        pointer
                    )
                }
                if status == noErr {
                    DispatchQueue.main.async {
                        HotKeyManager.shared.onToggleMute?()
                    }
                }
                return noErr
            },
            1,
            &eventType,
            nil,
            &handlerRef
        )

        guard handlerStatus == noErr else { return }

        var hotKeyID = EventHotKeyID()
        hotKeyID.signature = OSType(0x48594D4D) // 'HYMM'
        hotKeyID.id = 1

        let hotKeyStatus = RegisterEventHotKey(
            UInt32(kVK_ANSI_M),
            UInt32(controlKey | optionKey),
            hotKeyID,
            GetApplicationEventTarget(),
            0,
            &hotKeyRef
        )

        installed = hotKeyStatus == noErr
    }
}
