"""
Lightweight block-based reverb for the real-time audio callback.

A Schroeder/Freeverb-style reverb: four parallel feedback-comb filters
followed by two series allpass filters.  Delay lines are processed in
small blocks (32 frames) with NumPy, so the callback avoids per-sample
Python loops for the expensive part.

The reverb adds no algorithmic latency; it only adds CPU work to the
existing audio callback.
"""

import numpy as np


# Tunings from the classic Freeverb algorithm, expressed at 44.1 kHz.
_COMB_DELAYS = (1116, 1188, 1277, 1356)
_ALLPASS_DELAYS = (556, 441)


# name -> (room_size, damping, wet)
REVERB_PRESETS = {
    "room": (0.72, 0.45, 0.16),
    "hall": (0.84, 0.30, 0.24),
    "large": (0.91, 0.22, 0.30),
}


class _DelayLine:
    __slots__ = ("length", "buffer", "pos")

    def __init__(self, length):
        self.length = max(1, int(length))
        self.buffer = np.zeros(self.length, dtype=np.float32)
        self.pos = 0

    def read(self, n):
        """Read the n oldest samples and copy them (safe against writes)."""
        end = self.pos + n
        if end <= self.length:
            return self.buffer[self.pos:end].copy()
        return np.concatenate((
            self.buffer[self.pos:],
            self.buffer[:end - self.length],
        ))

    def write(self, block):
        n = block.size
        end = self.pos + n
        if end <= self.length:
            self.buffer[self.pos:end] = block
        else:
            first = self.length - self.pos
            self.buffer[self.pos:] = block[:first]
            self.buffer[:end - self.length] = block[first:]
        self.pos = end % self.length


class _Comb:
    __slots__ = ("delay", "feedback")

    def __init__(self, delay, feedback):
        self.delay = _DelayLine(delay)
        self.feedback = float(feedback)

    def process(self, block):
        delayed = self.delay.read(block.size)
        self.delay.write(block + delayed * self.feedback)
        return delayed


class _Allpass:
    __slots__ = ("delay", "feedback")

    def __init__(self, delay, feedback=0.5):
        self.delay = _DelayLine(delay)
        self.feedback = float(feedback)

    def process(self, block):
        delayed = self.delay.read(block.size)
        self.delay.write(block + delayed * self.feedback)
        return delayed - block


class Reverb:
    """Block-based reverb.  process() accepts mono float32 blocks."""

    def __init__(self, samplerate, room_size=0.8, damping=0.3, wet=0.25):
        scale = float(samplerate) / 44100.0
        self.combs = [
            _Comb(max(1, int(delay * scale)), room_size)
            for delay in _COMB_DELAYS
        ]
        self.allpasses = [
            _Allpass(max(1, int(delay * scale)))
            for delay in _ALLPASS_DELAYS
        ]
        self.wet = float(wet)
        self.damping = float(damping)
        self.comb_scale = 1.0 / len(self.combs)
        self.damp_state = 0.0
        # Keep every delay line longer than the processing block.
        self.max_block = 32

    def process(self, block):
        """Return the wet signal for a mono block."""
        n = block.size
        if n == 0:
            return block

        out = np.empty(n, dtype=np.float32)
        for start in range(0, n, self.max_block):
            chunk = block[start:start + self.max_block]
            m = chunk.size

            acc = np.zeros(m, dtype=np.float32)
            for comb in self.combs:
                acc += comb.process(chunk)
            acc *= self.comb_scale

            wet = acc
            for allpass in self.allpasses:
                wet = allpass.process(wet)

            if self.damping > 0.0:
                alpha = 1.0 - self.damping
                state = self.damp_state
                filtered = np.empty(m, dtype=np.float32)
                for i in range(m):
                    state = state * self.damping + wet[i] * alpha
                    filtered[i] = state
                self.damp_state = state
                wet = filtered

            out[start:start + m] = wet

        return out
