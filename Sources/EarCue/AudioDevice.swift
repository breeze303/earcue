import Foundation

struct AudioDevice: Identifiable, Hashable {
    let id: UInt32
    let name: String
    let uid: String
    let inputChannels: Int
    let outputChannels: Int
    let sampleRate: Double

    var isInput: Bool { inputChannels > 0 }
    var isOutput: Bool { outputChannels > 0 }
}
