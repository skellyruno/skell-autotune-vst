"""Optional full VST3 regression: pip install pedalboard numpy, then pass the bundle path.

Runs without an editor, so key/scale automation must work independently of the UI.
"""
import os
from pathlib import Path
import sys
import traceback

import numpy as np
from pedalboard import load_plugin

SR = 44100
BLOCK = 96
PLUGIN_PATH = Path(sys.argv[1]).resolve(strict=True)
# Pedalboard's Windows scanner expects the loadable VST3 module file. JUCE's
# Windows output is a standard bundle folder containing that module.
if sys.platform == "win32" and PLUGIN_PATH.is_dir():
    PLUGIN_PATH = PLUGIN_PATH / "Contents" / "x86_64-win" / (PLUGIN_PATH.stem + ".vst3")
PATH = str(PLUGIN_PATH)


def render(plugin, frequency):
    t = np.arange(SR * 2, dtype=np.float64) / SR
    voice = (0.2 * np.sin(2 * np.pi * frequency * t)).astype(np.float32)
    audio = np.stack((voice, voice))
    result = plugin(audio, SR, buffer_size=BLOCK)
    assert result.shape == audio.shape and np.isfinite(result).all()
    np.testing.assert_allclose(result[0], result[1], atol=1e-6)
    return result[0, SR:]


def assert_pitch(plugin, source, target):
    audio = render(plugin, source)
    spectrum = np.abs(np.fft.rfft(audio * np.hanning(len(audio))))
    peak = np.argmax(spectrum)
    logs = np.log(np.maximum(spectrum[peak - 1:peak + 2], 1e-20))
    offset = 0.5 * (logs[0] - logs[2]) / (logs[0] - 2 * logs[1] + logs[2])
    hz = (peak + offset) * SR / len(audio)
    cents = 1200 * np.log2(hz / target)
    assert abs(cents) < 8, (source, target, hz, cents)
    print(f"PASS {source:g} Hz -> {hz:.2f} Hz (target {target:.2f}, {cents:+.2f} cents)")


def main():
    for live in (False, True):
        plugin = load_plugin(PATH)
        plugin.low_latency_experimental = live
        plugin.scale_type = 1  # C minor: E must no longer be a permitted target.
        plugin.note_stabilizer = True  # This must not cancel pending scale updates.
        saved = plugin.raw_state  # Save before audio or a UI/message loop can run.
        assert_pitch(plugin, 336, 349.22823)  # Nearest C-minor target is F4, not E4.

        restored = load_plugin(PATH)
        restored.raw_state = saved
        assert not restored.e_on and restored.d_sharp_on and restored.a_sharp_on
        assert bool(restored.low_latency_experimental) == live
        assert_pitch(restored, 336, 349.22823)

        # An explicit piano edit remains custom and survives project reload.
        restored.f_on = False
        custom = restored.raw_state
        restored.root_note = 5
        restored.raw_state = custom
        assert not restored.f_on and not restored.e_on
        assert_pitch(restored, 336, 311.12698)  # D#4 is now nearest allowed note.

        # Changing the named key deliberately exits the custom mask, editor closed.
        restored.root_note = 2  # D minor: E4 and F4 allowed again.
        assert_pitch(restored, 336, 329.62756)
        reopened = load_plugin(PATH)
        reopened.raw_state = restored.raw_state
        assert reopened.f_on and reopened.e_on and not reopened.d_sharp_on
        print(f"PASS {'Live' if live else 'Standard'}: automation, custom piano, state round-trip, stereo, 44.1k/96")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        if os.environ.get("GITHUB_ACTIONS"):
            # Keep the failure readable in public job annotations as well as logs.
            details = traceback.format_exc().replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
            print("::error title=VST3 host regression::" + details, flush=True)
        raise
