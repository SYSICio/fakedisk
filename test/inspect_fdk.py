import struct, zlib

data = open(r"C:\Users\ncyc\Desktop\fakedisk\test.fdk", "rb").read()
print("file size:", len(data))
print("magic:", data[0:8])
ver, flags = struct.unpack_from("<HH", data, 8)
print("version:", ver, "flags:", flags)
label = data[12:44].split(b"\0")[0]
print("label:", label)
cap, toc_off, toc_size, data_off, data_size = struct.unpack_from("<QQQQQ", data, 44)
print("cap:", cap, "toc_off:", toc_off, "toc_size:", toc_size, "data_off:", data_off, "data_size:", data_size)
toc_crc = struct.unpack_from("<I", data, 84)[0]
hdr_crc = struct.unpack_from("<I", data, 88)[0]
print("toc_crc in header:", hex(toc_crc))
print("hdr_crc in header:", hex(hdr_crc))
toc = data[toc_off:toc_off+toc_size]
print("toc bytes:", toc.hex())
print("toc_crc recomputed:", hex(zlib.crc32(toc) & 0xFFFFFFFF))
print("hdr_crc recomputed (8..87):", hex(zlib.crc32(data[8:88]) & 0xFFFFFFFF))
print("toc bytes len:", len(toc))
# 手工解析 toc
o = 0
def rd(fmt):
    global o
    v = struct.unpack_from(fmt, toc, o)[0]
    o += struct.calcsize(fmt)
    return v
t = rd("B"); a = rd("B"); nl = rd("H")
print("root type:", t, "attr:", a, "name_len:", nl)
print("name:", toc[o:o+nl]); o += nl
print("created:", rd("q"), "modified:", rd("q"))
print("size:", rd("Q"), "data_off:", rd("Q"), "stored:", rd("I"), "comp:", rd("B"), "crc:", rd("I"), "children:", rd("I"))
print("consumed:", o, "of", len(toc))
