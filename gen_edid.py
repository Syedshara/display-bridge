#!/usr/bin/env python3
"""
gen_edid.py
Generate a valid EDID 1.4 binary (128 bytes) for a virtual display
with a Detailed Timing Descriptor for 2880x1800 @ 60 Hz.

Timing (CVT-RB derived):
  H active=2880  H blank=160   H total=3040
  V active=1800  V blank=30    V total=1830
  H front porch=48  H sync=32
  V front porch=3   V sync=6
  Pixel clock = 33379 x 10 kHz = 333.79 MHz  -> 59.997 Hz

HEVC conformance window: ALIGN(1800, 64)=1856, coded as 2880x1856,
  conf_win_bottom_offset=28 in SPS (crop 56 luma rows = 28 chroma units).
  This is handled automatically in encoder.c:write_sps().

Usage:
  python3 gen_edid.py | sudo tee /sys/kernel/debug/dri/0/Virtual-1/edid_override
"""

import sys


def build_edid() -> bytes:
    edid = bytearray(128)

    # ---- Header -------------------------------------------------------
    edid[0:8] = b'\x00\xff\xff\xff\xff\xff\xff\x00'

    # ---- Manufacturer ID "VRT" ----------------------------------------
    # Each char A-Z encodes as 1-26; packed into 15 bits (top bit=0), MSB first.
    # V=22, R=18, T=20
    val = ((22 << 10) | (18 << 5) | 20) & 0x7FFF
    edid[8] = (val >> 8) & 0xFF   # 0x5A
    edid[9] = val & 0xFF          # 0x54

    # Product code (little-endian), serial (unused)
    edid[10] = 0x01
    edid[11] = 0x00
    edid[12:16] = b'\x00\x00\x00\x00'

    # Week=1, Year=2024 (2024-1990=34)
    edid[16] = 1
    edid[17] = 34

    # EDID version 1.4
    edid[18] = 1
    edid[19] = 4

    # Video input: digital, 8 bpc, DisplayPort
    edid[20] = 0b10100101  # 0xA5

    # H/V screen size: 34 cm x 21 cm (~15.4" diagonal)
    edid[21] = 34
    edid[22] = 21

    # Gamma 2.2 -> (2.2*100 - 100) = 120 = 0x78
    edid[23] = 0x78

    # Feature support: preferred timing in descriptor 1
    edid[24] = 0x02

    # Chromaticity (sRGB primaries, 10-bit values packed per EDID spec)
    # Rx=654, Ry=338, Gx=307, Gy=614, Bx=154, By=61, Wx=320, Wy=337
    edid[25:35] = bytes([0xAE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])

    # Established timings I/II/mfr: all zero
    edid[35] = 0x00
    edid[36] = 0x00
    edid[37] = 0x00

    # Standard timing info: 8 slots, all unused (0x01 0x01 per spec)
    for i in range(8):
        edid[38 + i * 2 + 0] = 0x01
        edid[38 + i * 2 + 1] = 0x01

    # ---- Descriptor 1: Detailed Timing 2880x1800@60Hz -----------------
    pclk = 33379      # pixel clock in 10 kHz units = 333.79 MHz
    ha, hb  = 2880, 160    # H active, H blank
    hfp, hsw = 48, 32     # H front porch, H sync width
    va, vb  = 1800, 30    # V active, V blank
    vfp, vsw = 3, 6       # V front porch, V sync width
    hsz, vsz = 344, 215   # image size in mm

    dtd = bytearray(18)
    dtd[0]  = pclk & 0xFF
    dtd[1]  = (pclk >> 8) & 0xFF
    dtd[2]  = ha & 0xFF
    dtd[3]  = hb & 0xFF
    dtd[4]  = ((ha >> 8) & 0x0F) << 4 | ((hb >> 8) & 0x0F)
    dtd[5]  = va & 0xFF
    dtd[6]  = vb & 0xFF
    dtd[7]  = ((va >> 8) & 0x0F) << 4 | ((vb >> 8) & 0x0F)
    dtd[8]  = hfp & 0xFF
    dtd[9]  = hsw & 0xFF
    dtd[10] = ((vfp & 0x0F) << 4) | (vsw & 0x0F)
    dtd[11] = (((hfp >> 8) & 0x03) << 6) | (((hsw >> 8) & 0x03) << 4) | \
              (((vfp >> 4) & 0x03) << 2) | ((vsw >> 4) & 0x03)
    dtd[12] = hsz & 0xFF
    dtd[13] = vsz & 0xFF
    dtd[14] = ((hsz >> 8) & 0x0F) << 4 | ((vsz >> 8) & 0x0F)
    dtd[15] = 0    # H border
    dtd[16] = 0    # V border
    dtd[17] = 0x1E  # digital separate sync, +H +V polarity
    edid[54:72] = dtd

    # ---- Descriptor 2: Monitor Range Limits (tag 0xFD) ----------------
    # H line rate = 333790000 / 3040 = 109.8 kHz  -> range 80-130 kHz
    # Max pixel clock 333.8 MHz -> 340 MHz -> 34 (units of 10 MHz)
    rng = bytearray(18)
    rng[0:4] = b'\x00\x00\x00\xfd'
    rng[4]  = 0x00
    rng[5]  = 24     # min V rate Hz
    rng[6]  = 75     # max V rate Hz
    rng[7]  = 80     # min H rate kHz
    rng[8]  = 130    # max H rate kHz
    rng[9]  = 34     # max pixel clock x10 MHz = 340 MHz
    rng[10] = 0x00   # no secondary timing curve
    rng[11:18] = b'\x0a\x20\x20\x20\x20\x20\x20'
    edid[72:90] = rng

    # ---- Descriptor 3: Monitor Name (tag 0xFC) ------------------------
    nam = bytearray(18)
    nam[0:4] = b'\x00\x00\x00\xfc'
    nam[4]   = 0x00
    name_bytes = b'VirtDisplay\n'   # 11 chars + LF = 12, padded to 13 with 0x20
    nam[5:5 + len(name_bytes)] = name_bytes
    for i in range(5 + len(name_bytes), 18):
        nam[i] = 0x20
    edid[90:108] = nam

    # ---- Descriptor 4: Dummy (tag 0x10) --------------------------------
    edid[108:126] = b'\x00\x00\x00\x10' + b'\x00' * 14

    # ---- Extensions: none ----------------------------------------------
    edid[126] = 0

    # ---- Checksum -------------------------------------------------------
    edid[127] = (256 - (sum(edid[:127]) % 256)) % 256

    return bytes(edid)


def verify(data: bytes) -> None:
    assert len(data) == 128, f"EDID must be 128 bytes, got {len(data)}"
    assert data[0:8] == b'\x00\xff\xff\xff\xff\xff\xff\x00', "Bad header"
    assert sum(data) % 256 == 0, f"Checksum invalid (sum={sum(data)} mod 256={sum(data)%256})"
    pclk = data[54] | (data[55] << 8)
    ha   = data[56] | ((data[58] >> 4) << 8)
    va   = data[59] | ((data[61] >> 4) << 8)
    ht   = ha + (data[57] | ((data[58] & 0x0F) << 8))
    vt   = va + (data[60] | ((data[61] & 0x0F) << 8))
    fps  = (pclk * 10000) / (ht * vt)
    sys.stderr.write(f"EDID OK: {ha}x{va} H_total={ht} V_total={vt} "
                     f"pclk={pclk*10}kHz  fps={fps:.3f}\n")


if __name__ == '__main__':
    data = build_edid()
    verify(data)
    sys.stdout.buffer.write(data)
