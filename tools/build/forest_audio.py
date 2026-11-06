#!/usr/bin/env python3
"""Generates a 3-second forest ambience (wind, leaf rustle, birdsong) as audio.wav.
Pure Python, no extra packages needed."""
import math, random, struct, wave, sys

RATE = 32728          # sample rate the 3DS banner format likes
DUR = 3.0             # banner audio must be 3 seconds or less
N = int(RATE * DUR)
random.seed(7)        # same result every run; change the number for a different mix

out = [0.0] * N

# 1) Wind: noise through two low-pass filters, slowly swelling
lp1 = lp2 = 0.0
for i in range(N):
    t = i / RATE
    n = random.uniform(-1, 1)
    lp1 += 0.03 * (n - lp1)
    lp2 += 0.05 * (lp1 - lp2)
    swell = 0.6 + 0.4 * math.sin(2 * math.pi * 0.45 * t + 1.0)
    out[i] += lp2 * 5.0 * swell * 0.55

# 2) Leaf rustle: soft high-passed noise bursts
hp_prev_n = hp = 0.0
env = 0.0
for i in range(N):
    if random.random() < 0.0006:
        env = random.uniform(0.3, 0.8)
    env *= 0.9992
    n = random.uniform(-1, 1)
    hp = 0.9 * (hp + n - hp_prev_n)
    hp_prev_n = n
    out[i] += hp * env * 0.10

# 3) Birdsong: short chirps with a pitch sweep and a little vibrato
def chirp(start, length, f0, f1, vol, vib=0.0):
    s0 = int(start * RATE)
    L = int(length * RATE)
    phase = 0.0
    for k in range(L):
        j = s0 + k
        if j >= N:
            break
        p = k / L
        f = f0 + (f1 - f0) * p + vib * math.sin(2 * math.pi * 28 * k / RATE)
        phase += 2 * math.pi * f / RATE
        e = math.sin(math.pi * p) ** 2           # smooth attack and decay
        out[j] += math.sin(phase) * e * vol

# bird A: quick rising "tweet-tweet-tweet"
for k in range(3):
    chirp(0.35 + k * 0.13, 0.09, 2600, 3600, 0.22)
# bird B: warbling trill
chirp(1.15, 0.42, 3200, 3000, 0.16, vib=260)
# bird C: two-note descending call
chirp(1.85, 0.16, 3900, 3000, 0.20)
chirp(2.05, 0.20, 3300, 2500, 0.20)

# fade in/out so it never clicks, then normalize
fade = int(0.25 * RATE)
for i in range(fade):
    g = i / fade
    out[i] *= g
    out[N - 1 - i] *= g
peak = max(abs(x) for x in out) or 1.0
scale = 0.85 / peak

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else "audio.wav", "wb")
w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE)
w.writeframes(b"".join(struct.pack("<h", int(x * scale * 32767)) for x in out))
w.close()
print("audio.wav written (%.1fs)" % DUR)
