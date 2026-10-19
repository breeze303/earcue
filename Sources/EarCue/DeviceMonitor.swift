import CoreAudio
import Foundation

/// Native Core Audio device-change notifications.
/// Replaces periodic polling with property listeners on the system object.
final class DeviceMonitor {
    struct Listener {
        var address: AudioObjectPropertyAddress
        let block: AudioObjectPropertyListenerBlock
    }

    var onChange: (() -> Void)?

    private let queue = DispatchQueue.main
    private var listeners: [Listener] = []

    @discardableResult
    func start() -> Bool {
        let selectors: [AudioObjectPropertySelector] = [
            kAudioHardwarePropertyDevices,
            kAudioHardwarePropertyDefaultInputDevice,
            kAudioHardwarePropertyDefaultOutputDevice
        ]

        var allInstalled = true
        for selector in selectors {
            var address = AudioObjectPropertyAddress(
                mSelector: selector,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: 0
            )
            let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
                self?.onChange?()
            }
            let status = AudioObjectAddPropertyListenerBlock(
                AudioObjectID(kAudioObjectSystemObject),
                &address,
                queue,
                block
            )
            if status == noErr {
                listeners.append(Listener(address: address, block: block))
            } else {
                allInstalled = false
            }
        }
        return allInstalled
    }

    deinit {
        for listener in listeners {
            var address = listener.address
            AudioObjectRemovePropertyListenerBlock(
                AudioObjectID(kAudioObjectSystemObject),
                &address,
                queue,
                listener.block
            )
        }
    }
}
