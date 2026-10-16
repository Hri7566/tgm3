"""Minimal PE32 reader: sections, imports, and an image view addressed by VA."""
import struct


class Image:
    def __init__(self, path):
        data = open(path, "rb").read()
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        assert data[:2] == b"MZ" and data[pe:pe + 4] == b"PE\0\0"
        coff = pe + 4
        nsec, opt_size = struct.unpack_from("<H", data, coff + 2)[0], struct.unpack_from("<H", data, coff + 16)[0]
        opt = coff + 20
        self.base = struct.unpack_from("<I", data, opt + 28)[0]
        self.size = struct.unpack_from("<I", data, opt + 56)[0]
        self.entry = self.base + struct.unpack_from("<I", data, opt + 16)[0]
        headers = struct.unpack_from("<I", data, opt + 60)[0]
        self.mem = bytearray(self.size)
        self.mem[:headers] = data[:headers]
        self.sections = []
        s = opt + opt_size
        for _ in range(nsec):
            name = data[s:s + 8].rstrip(b"\0").decode()
            vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
            chars = struct.unpack_from("<I", data, s + 36)[0]
            self.mem[vaddr:vaddr + rsize] = data[raw:raw + rsize]
            self.sections.append((name, self.base + vaddr, vsize, chars))
            s += 40
        import_rva = struct.unpack_from("<I", data, opt + 96 + 8)[0]
        self.imports = {}            # IAT slot VA -> (dll, name)
        d = import_rva
        while struct.unpack_from("<I", self.mem, d + 12)[0]:
            lookup, _, _, name_rva, iat = struct.unpack_from("<IIIII", self.mem, d)
            dll = self.cstr(self.base + name_rva)
            lookup = lookup or iat
            i = 0
            while True:
                v = struct.unpack_from("<I", self.mem, lookup + 4 * i)[0]
                if not v:
                    break
                name = "#%d" % (v & 0xFFFF) if v & 0x80000000 else self.cstr(self.base + v + 2)
                self.imports[self.base + iat + 4 * i] = (dll.lower(), name)
                i += 1
            d += 20
        text = [s for s in self.sections if s[0] == ".text"][0]
        self.text_lo, self.text_hi = text[1], text[1] + text[2]

    def in_text(self, va):
        return self.text_lo <= va < self.text_hi

    def in_image(self, va):
        return self.base <= va < self.base + self.size

    def bytes(self, va, n):
        o = va - self.base
        return bytes(self.mem[o:o + n])

    def u32(self, va):
        return struct.unpack_from("<I", self.mem, va - self.base)[0]

    def u8(self, va):
        return self.mem[va - self.base]

    def cstr(self, va):
        o = va - self.base
        e = self.mem.index(0, o)
        return self.mem[o:e].decode("latin-1")
