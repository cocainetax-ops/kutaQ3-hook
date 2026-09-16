#!/usr/bin/env python3
"""Regenerate the deflate fixtures tests/test_nameesp.cpp embeds for WEAPON ESP's icon pipeline.

The payloads are MakeIcon()'s bytes from that file, byte for byte: 32bpp B,G,R,A, bottom-up.
zlib gives us a real raw-DEFLATE (ZIP method 8) stream of each deflate block type, so the tests
can push the loader's inflater through all three.
"""
import zlib


def make_icon(w, h):
    hdr = bytearray(18)
    hdr[2] = 2
    hdr[12] = w & 0xFF
    hdr[13] = w >> 8
    hdr[14] = h & 0xFF
    hdr[15] = h >> 8
    hdr[16] = 32          # pixel depth
    hdr[17] = 0           # bottom-up
    rad = max(1, w // 2 - 2)
    cx, cy = w // 2, h // 2
    px = bytearray()
    for f in range(h):                    # the file's row order: bottom-up, so the image's
        y = h - 1 - f                     # first row is the file's last one
        row = bytearray()
        for x in range(w):
            dx, dy = x - cx, y - cy
            if dx * dx + dy * dy < rad * rad:
                r = (x * 8) & 0xFF
                g = (y * 8) & 0xFF
                b = ((x + y) * 4) & 0xFF
                a = 255
            else:
                r = g = b = a = 0
            row += bytes((b, g, r, a))
        px += row
    return bytes(hdr) + bytes(px)


def deflate(data, level):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)     # -15 = raw DEFLATE
    return c.compress(data) + c.flush()


def emit(name, data, comment):
    print(comment)
    print('static const unsigned char %s[] = {' % name)
    for i in range(0, len(data), 12):
        print('\t' + ', '.join('0x%02x' % b for b in data[i:i + 12]) + ',')
    print('};')
    print()


def main():
    print('// The deflate fixtures: zlib 1.2.13, raw -15 windows, generated from MakeIcon()\'s bytes (so')
    print('// a test can inflate one and compare it against the TGA it builds itself). They cover the')
    print('// three DEFLATE block types, and TestWeaponIconPakRead() asserts each fixture\'s block type')
    print('// so a regeneration with different settings cannot quietly drop one. Regenerate with')
    print('// tests/gen_icon_fixtures.py (kept next to the tests for exactly that).')
    print()
    emit('kDeflateIconDynamic', deflate(make_icon(32, 32), 9),
         '// 32x32, level 9: what a pak packager produces - a DYNAMIC Huffman block.')
    emit('kDeflateIconStored', deflate(make_icon(8, 8), 0),
         '// 8x8, level 0: STORED deflate blocks - a method-8 entry that is not Huffman coded at all.')
    emit('kDeflateIconFixed', deflate(make_icon(4, 4), 1),
         '// 4x4, level 1: small enough that zlib picks the FIXED Huffman table.')


if __name__ == '__main__':
    main()
