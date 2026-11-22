"""Synthesise the task-complete chime: two soft bell notes, rising.

    python chime.py out.wav

Kenshi's own sounds are Wwise events played by name, and the names aren't
findable (the banks hash them), so the plugin plays this file itself with
PlaySound. It doesn't follow Kenshi's volume sliders, so it's kept quiet.
"""
import math
import struct
import sys
import wave

RATE = 44100
PEAK = 0.28  # about -11 dBFS


def bell(t, freq):
    # A few inharmonic partials, the higher ones dying faster: reads as a bell.
    partials = ((1.0, 1.0, 0.45), (2.0, 0.35, 0.25), (2.76, 0.18, 0.15), (5.4, 0.06, 0.08))
    v = 0.0
    for mult, amp, tau in partials:
        v += amp * math.exp(-t / tau) * math.sin(2 * math.pi * freq * mult * t)
    attack = min(1.0, t / 0.004)
    return v * attack


def render(seconds=1.1):
    notes = ((0.0, 659.25), (0.11, 987.77))  # E5, then B5
    out = []
    for i in range(int(RATE * seconds)):
        t = i / RATE
        v = sum(bell(t - start, f) for start, f in notes if t >= start)
        out.append(v)
    top = max(abs(v) for v in out)
    return [v / top * PEAK for v in out]


def write(path):
    samples = render()
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(v * 32767)) for v in samples))
    return path


if __name__ == "__main__":
    print("wrote", write(sys.argv[1]))
