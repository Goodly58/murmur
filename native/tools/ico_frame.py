"""Extracts one PNG frame from an .ico: ico_frame.py <ico> <size> <out.png>"""
import struct
import sys

data = open(sys.argv[1], "rb").read()
want = int(sys.argv[2])
_, _, count = struct.unpack_from("<HHH", data, 0)
for i in range(count):
    w, h, _c, _r, _p, _b, size, off = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
    w = w or 256
    if w == want:
        open(sys.argv[3], "wb").write(data[off:off + size])
        print(f"frame {w}x{h}: {size} bytes -> {sys.argv[3]}")
        break
else:
    print("no such size")
