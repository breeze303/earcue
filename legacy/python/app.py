#!/usr/bin/env python3
"""
Audio Monitor - Menu Bar App
Hear your microphone through your speakers with minimal latency.

Made with love by Gabrycina & Claude Opus
"""

import rumps
import sounddevice as sd
import numpy as np
import os
import sys
import fcntl

from reverb import Reverb, REVERB_PRESETS

# Single instance lock file
LOCK_FILE = os.path.expanduser("~/.audiomonitor.lock")


def ensure_single_instance():
    """Ensure only one instance of the app is running."""
    global lock_file_handle
    lock_file_handle = open(LOCK_FILE, 'w')
    try:
        fcntl.flock(lock_file_handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Write PID to lock file
        lock_file_handle.write(str(os.getpid()))
        lock_file_handle.flush()
        return True
    except IOError:
        # Another instance is running
        return False


def list_audio_devices():
    """Return the current PortAudio devices, or an empty list on failure."""
    try:
        return list(sd.query_devices())
    except Exception as e:
        print(f"Could not query audio devices: {e}", file=sys.stderr)
        return []


def reinitialize_audio():
    """
    Restart PortAudio so hot-plugged devices become visible.

    PortAudio builds its device list during Pa_Initialize() and keeps it
    frozen until Pa_Terminate()/Pa_Initialize(). sounddevice's private
    helpers are used because it does not expose a public refresh call.
    Must only be called while no stream is open.
    """
    terminate = getattr(sd, "_terminate", None)
    initialize = getattr(sd, "_initialize", None)
    if terminate is None or initialize is None:
        return
    try:
        terminate()
    except Exception as e:
        print(f"Could not terminate PortAudio: {e}", file=sys.stderr)
    try:
        initialize()
    except Exception as e:
        print(f"Could not initialize PortAudio: {e}", file=sys.stderr)


def close_stream(stream):
    """Stop and close a sounddevice stream, ignoring cleanup errors."""
    if stream is None:
        return
    try:
        stream.stop()
    except Exception as e:
        print(f"Could not stop audio stream: {e}", file=sys.stderr)
    try:
        stream.close()
    except Exception as e:
        print(f"Could not close audio stream: {e}", file=sys.stderr)


def device_signature(devices):
    """Cheap snapshot used to detect devices being plugged in or removed."""
    return tuple(
        (
            d["index"],
            d["name"],
            d["max_input_channels"],
            d["max_output_channels"],
            int(d["default_samplerate"]),
        )
        for d in devices
    )


def devices_for(devices, kind):
    """Return the devices usable as input or output."""
    key = "max_input_channels" if kind == "input" else "max_output_channels"
    return [d for d in devices if d[key] > 0]


def device_name(devices, index):
    """Return a device's name by index, or None."""
    for d in devices:
        if d["index"] == index:
            return d["name"]
    return None


def find_device(devices, index, name, kind):
    """
    Re-find a selected device after the device list has changed.

    PortAudio indices are not stable when devices are plugged in or removed,
    so match on the name first and only then fall back to the index.
    """
    candidates = devices_for(devices, kind)
    if not candidates:
        return None, None
    if name is not None:
        for d in candidates:
            if d["index"] == index and d["name"] == name:
                return d["index"], d["name"]
        for d in candidates:
            if d["name"] == name:
                return d["index"], d["name"]
    if index is not None:
        for d in candidates:
            if d["index"] == index:
                return d["index"], d["name"]
    return candidates[0]["index"], candidates[0]["name"]


class AudioMonitorApp(rumps.App):
    def __init__(self):
        super().__init__("Audio Monitor", icon=None, quit_button=None)

        self.stream = None
        self.is_running = False
        self.blocksize = 32
        self.samplerate = 48000
        self.reverb = None
        self.reverb_preset = "off"

        # Snapshot of the current devices (also used to build the menu).
        self.devices = list_audio_devices()
        self._device_signature = device_signature(self.devices)

        # Get default devices. find_device() falls back to the first available
        # device if the system default is missing or has index -1.
        try:
            default_input, default_output = sd.default.device
        except Exception:
            default_input = default_output = None

        self.input_device, self.input_device_name = find_device(
            self.devices, default_input, None, "input"
        )
        self.output_device, self.output_device_name = find_device(
            self.devices, default_output, None, "output"
        )

        # Build menu
        self.build_menu()

        # Update icon
        self.update_title()

        # Show welcome notification after a brief delay
        rumps.Timer(self.show_welcome, 1).start()

        # Poll for devices being plugged in/unplugged. Polling (instead of a
        # CoreAudio listener) keeps this dependency-free and works for both
        # the built-in jack and USB audio interfaces.
        self.device_timer = rumps.Timer(self.refresh_devices, 2)
        self.device_timer.start()

    def build_menu(self):
        """Build the menu with current devices."""
        self.menu.clear()

        # Status / Toggle
        if self.is_running:
            self.menu.add(rumps.MenuItem("● Monitoring Active", callback=None))
            self.menu.add(rumps.MenuItem("Stop Monitoring", callback=self.toggle_monitoring))
            if self.stream is not None:
                try:
                    input_latency, output_latency = self.stream.latency
                    self.menu.add(rumps.MenuItem(
                        f"Latency: {input_latency * 1000:.1f} + "
                        f"{output_latency * 1000:.1f} ms",
                        callback=None
                    ))
                except Exception:
                    pass
        else:
            self.menu.add(rumps.MenuItem("○ Monitoring Stopped", callback=None))
            self.menu.add(rumps.MenuItem("Start Monitoring", callback=self.toggle_monitoring))

        self.menu.add(rumps.separator)

        # Input devices submenu
        input_menu = rumps.MenuItem("Input Device")
        input_devices = self.get_input_devices()
        if not input_devices:
            input_menu.add(rumps.MenuItem("(No input devices found)", callback=None))
        for idx, name in input_devices:
            item = rumps.MenuItem(
                f"{'✓ ' if idx == self.input_device else '   '}{name}",
                callback=lambda sender, i=idx: self.set_input_device(i)
            )
            input_menu.add(item)
        self.menu.add(input_menu)

        # Output devices submenu
        output_menu = rumps.MenuItem("Output Device")
        output_devices = self.get_output_devices()
        if not output_devices:
            output_menu.add(rumps.MenuItem("(No output devices found)", callback=None))
        for idx, name in output_devices:
            item = rumps.MenuItem(
                f"{'✓ ' if idx == self.output_device else '   '}{name}",
                callback=lambda sender, i=idx: self.set_output_device(i)
            )
            output_menu.add(item)
        self.menu.add(output_menu)

        # Device lists can change while the app is running (e.g. plugging in
        # 3.5mm speakers). Allow an immediate manual refresh as well.
        self.menu.add(rumps.MenuItem("Refresh Devices", callback=self._manual_refresh))

        self.menu.add(rumps.separator)

        # Latency settings
        latency_menu = rumps.MenuItem("Latency")
        latencies = [
            (16, "16 frames (experimental, ~0.33 ms buffer)"),
            (32, "32 frames (~0.67 ms buffer)"),
            (64, "64 frames (~1.33 ms buffer)"),
            (128, "128 frames (~2.67 ms buffer)"),
            (256, "256 frames (~5.33 ms buffer)"),
        ]
        for blocksize, label in latencies:
            item = rumps.MenuItem(
                f"{'✓ ' if blocksize == self.blocksize else '   '}{label}",
                callback=lambda sender, b=blocksize: self.set_latency(b)
            )
            latency_menu.add(item)
        self.menu.add(latency_menu)

        # Reverb settings
        reverb_menu = rumps.MenuItem("Reverb")
        reverb_options = [
            ("off", "Off"),
            ("room", "Room"),
            ("hall", "Hall"),
            ("large", "Large Hall"),
        ]
        for preset, label in reverb_options:
            item = rumps.MenuItem(
                f"{'✓ ' if preset == self.reverb_preset else '   '}{label}",
                callback=lambda sender, p=preset: self.set_reverb(p)
            )
            reverb_menu.add(item)
        self.menu.add(reverb_menu)

        self.menu.add(rumps.separator)

        # About
        self.menu.add(rumps.MenuItem("About", callback=self.show_about))
        self.menu.add(rumps.MenuItem("Quit", callback=self.quit_app))

    def get_input_devices(self):
        """Get list of input devices."""
        return [(d["index"], d["name"]) for d in devices_for(self.devices, "input")]

    def get_output_devices(self):
        """Get list of output devices."""
        return [(d["index"], d["name"]) for d in devices_for(self.devices, "output")]

    def update_title(self):
        """Update menu bar title/icon."""
        if self.is_running:
            self.title = "🎙"
        else:
            self.title = "🎙️"  # Slightly different to show state (or use 🔇)

    def audio_callback(self, indata, outdata, frames, time, status):
        """Direct passthrough callback.

        Input is mixed down to mono and copied to every output channel, so
        mono microphones work with stereo outputs (and vice versa).
        """
        if status:
            print(status)
        if indata.shape[1] == 1:
            mono = indata[:, 0]
        else:
            mono = indata.mean(axis=1)

        # Take a local reference: the menu thread may replace self.reverb
        # (or set it to None) while this callback is running.
        reverb = self.reverb
        if reverb is not None:
            wet = reverb.process(mono)
            wet_mix = reverb.wet
            mono = mono * (1.0 - wet_mix) + wet * wet_mix

        # Safety net: never let a hot input or a long reverb tail overflow
        # the output buffers.
        mono = np.clip(mono, -1.0, 1.0)
        outdata[:] = mono[:, None]

    def _choose_stream_settings(self):
        """Find a sample rate / channel layout supported by both devices."""
        in_info = sd.query_devices(self.input_device)
        out_info = sd.query_devices(self.output_device)

        input_options = [
            ch for ch in (1, 2) if in_info["max_input_channels"] >= ch
        ] or [1]
        output_options = [
            ch for ch in (2, 1) if out_info["max_output_channels"] >= ch
        ] or [1]

        rates = []
        for samplerate in (
            self.samplerate,
            48000,
            44100,
            int(in_info["default_samplerate"]),
            int(out_info["default_samplerate"]),
        ):
            if samplerate and samplerate not in rates:
                rates.append(samplerate)

        errors = []
        for samplerate in rates:
            for input_channels in input_options:
                for output_channels in output_options:
                    try:
                        sd.check_input_settings(
                            device=self.input_device,
                            samplerate=samplerate,
                            channels=input_channels,
                            dtype=np.float32,
                        )
                        sd.check_output_settings(
                            device=self.output_device,
                            samplerate=samplerate,
                            channels=output_channels,
                            dtype=np.float32,
                        )
                        return samplerate, input_channels, output_channels
                    except Exception as e:
                        errors.append(
                            f"{samplerate} Hz, {input_channels} in / "
                            f"{output_channels} out: {e}"
                        )

        raise RuntimeError(
            "No compatible sample rate / channel layout found.\n"
            + "\n".join(errors[-5:])
        )

    def _build_reverb(self, samplerate):
        """Create the reverb for the actual stream sample rate."""
        if self.reverb_preset == "off" or self.reverb_preset not in REVERB_PRESETS:
            self.reverb = None
            return
        room_size, damping, wet = REVERB_PRESETS[self.reverb_preset]
        self.reverb = Reverb(samplerate, room_size, damping, wet)

    def start_stream(self):
        """Start the audio stream."""
        if self.input_device is None or self.output_device is None:
            rumps.alert(
                "Error",
                "No audio input or output device is available.\n"
                "Plug in a device and choose 'Refresh Devices'."
            )
            return

        try:
            samplerate, input_channels, output_channels = self._choose_stream_settings()
            self._build_reverb(samplerate)
            self.stream = sd.Stream(
                device=(self.input_device, self.output_device),
                samplerate=samplerate,
                blocksize=self.blocksize,
                dtype=np.float32,
                latency=0.001,
                channels=(input_channels, output_channels),
                callback=self.audio_callback
            )
            self.stream.start()
            self.is_running = True
            self.update_title()
            self.build_menu()
        except Exception as e:
            if self.stream is not None:
                close_stream(self.stream)
                self.stream = None
            self.reverb = None
            self.is_running = False
            self.update_title()
            self.build_menu()
            rumps.alert(
                "Error",
                f"Could not start audio stream:\n"
                f"Input: {self.input_device_name or '?'}\n"
                f"Output: {self.output_device_name or '?'}\n\n{e}"
            )

    def stop_stream(self):
        """Stop the audio stream."""
        if self.stream:
            close_stream(self.stream)
            self.stream = None
        self.reverb = None
        self.is_running = False
        self.update_title()
        self.build_menu()

    def toggle_monitoring(self, _):
        """Toggle monitoring on/off."""
        if self.is_running:
            self.stop_stream()
        else:
            self.start_stream()

    def set_input_device(self, device_idx):
        """Set input device."""
        was_running = self.is_running
        if was_running:
            self.stop_stream()
        self.input_device = device_idx
        self.input_device_name = device_name(self.devices, device_idx)
        self.build_menu()
        if was_running:
            self.start_stream()

    def set_output_device(self, device_idx):
        """Set output device."""
        was_running = self.is_running
        if was_running:
            self.stop_stream()
        self.output_device = device_idx
        self.output_device_name = device_name(self.devices, device_idx)
        self.build_menu()
        if was_running:
            self.start_stream()

    def set_latency(self, blocksize):
        """Set latency (blocksize)."""
        was_running = self.is_running
        if was_running:
            self.stop_stream()
        self.blocksize = blocksize
        self.build_menu()
        if was_running:
            self.start_stream()

    def set_reverb(self, preset):
        """Switch reverb preset without restarting the audio stream."""
        self.reverb_preset = preset
        if self.is_running and self.stream is not None:
            try:
                self._build_reverb(self.stream.samplerate)
            except Exception as e:
                self.reverb = None
                print(f"Could not create reverb: {e}", file=sys.stderr)
        self.build_menu()

    def _manual_refresh(self, _=None):
        """Refresh the menu immediately (used by the Refresh Devices item)."""
        self.refresh_devices(force=True)

    def refresh_devices(self, _=None, force=False):
        """Pick up devices that were plugged in or removed since the last check."""
        # PortAudio freezes its device list when it is initialized, so simply
        # calling query_devices() will never see a device that was connected
        # later. Reinitialize PortAudio while no stream is open.
        if self.is_running and not force:
            return

        restart = self.is_running
        if restart:
            self.stop_stream()

        reinitialize_audio()
        devices = list_audio_devices()
        signature = device_signature(devices)

        if signature == self._device_signature:
            if restart:
                self.start_stream()
            elif force:
                self.build_menu()
            return

        old_input, old_input_name = self.input_device, self.input_device_name
        old_output, old_output_name = self.output_device, self.output_device_name

        self.devices = devices
        self._device_signature = signature

        self.input_device, self.input_device_name = find_device(
            devices, old_input, old_input_name, "input"
        )
        self.output_device, self.output_device_name = find_device(
            devices, old_output, old_output_name, "output"
        )

        input_present = old_input_name is not None and any(
            d["name"] == old_input_name for d in devices_for(devices, "input")
        )
        output_present = old_output_name is not None and any(
            d["name"] == old_output_name for d in devices_for(devices, "output")
        )

        # If the selected device was unplugged, follow the system default
        # (for example, macOS switches output to the 3.5mm headphones).
        try:
            default_input, default_output = sd.default.device
        except Exception:
            default_input = default_output = None
        if not input_present:
            self.input_device, self.input_device_name = find_device(
                devices, default_input, None, "input"
            )
        if not output_present:
            self.output_device, self.output_device_name = find_device(
                devices, default_output, None, "output"
            )

        if restart and input_present and output_present:
            # Devices were added, or the selected devices only moved to new
            # indices: it is safe to resume monitoring.
            self.start_stream()
        else:
            self.build_menu()
            if restart and not (input_present and output_present):
                # Do not silently switch to another output (for example from
                # headphones to the built-in speakers) when a device disappears.
                try:
                    rumps.notification(
                        title="Audio device disconnected",
                        subtitle="Monitoring stopped",
                        message="Choose a new input/output device from the menu.",
                        sound=False
                    )
                except Exception:
                    pass

    def show_welcome(self, _=None):
        """Show welcome notification pointing to menu bar."""
        try:
            rumps.notification(
                title="Audio Monitor is running!",
                subtitle="Look for 🎙 in your menu bar",
                message="Click the microphone icon to start monitoring and change settings.",
                sound=False
            )
        except Exception:
            pass  # Notification may fail in some environments, that's ok

    def show_about(self, _):
        """Show about dialog."""
        rumps.alert(
            title="Hear Yourself",
            message="Hear your microphone through your speakers\nwith minimal latency.\n\n"
                    "Made with ♥ by Gabrycina & Claude Opus\n\n"
                    "github.com/gabrycina/hear-yourself",
            ok="Nice!"
        )

    def quit_app(self, _):
        """Quit the application."""
        self.stop_stream()
        try:
            self.device_timer.stop()
        except Exception:
            pass
        # Release lock file
        try:
            fcntl.flock(lock_file_handle, fcntl.LOCK_UN)
            lock_file_handle.close()
            os.remove(LOCK_FILE)
        except:
            pass
        rumps.quit_application()


if __name__ == "__main__":
    if not ensure_single_instance():
        # Already running - show alert and exit
        rumps.alert(
            title="Audio Monitor Already Running",
            message="Look for 🎙 in your menu bar!\n\nThe app is already running. Click the microphone icon to access settings.",
            ok="Got it!"
        )
        sys.exit(0)

    app = AudioMonitorApp()
    app.run()
