#!/usr/bin/env python3
"""Spectrogram PNG of a WAV (left channel): spec.py in.wav out.png [start_s] [dur_s]"""
import sys, wave, numpy as np
from PIL import Image
w = wave.open(sys.argv[1]); sr = w.getframerate()
a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).reshape(-1, 2)[:, 0].astype(np.float32)
s = float(sys.argv[3]) if len(sys.argv) > 3 else 0; d = float(sys.argv[4]) if len(sys.argv) > 4 else 20
a = a[int(s * sr):int((s + d) * sr)]
N, hop = 2048, 512
fr = np.lib.stride_tricks.sliding_window_view(a, N)[::hop] * np.hanning(N)
m = np.abs(np.fft.rfft(fr, axis=1))[:, :400]            # up to ~7.8 kHz
db = 20 * np.log10(m + 1); db = np.clip((db - db.max() + 70) / 70, 0, 1)
Image.fromarray((db.T[::-1] * 255).astype(np.uint8)).resize((1200, 400)).save(sys.argv[2])
