"""KID engine (Ever17 / Never7 PC) file formats.

Ported from Ever17Converter by weeaboo (GPLv2, nl.weeaboo.kid): UnDAT.java (LNK archives,
LND compression), KIDResourceConverter.java (CPS/PRT images) and WafToWav.java (voices).
This file is GPLv2 as well.
"""
import os
import struct


# ---------------------------------------------------------------- LNK archives (.dat)

def lnd_decompress(src, pos, length):
    """KID 'lnd' decompression of `length` bytes starting at src[pos]. Returns bytes."""
    out = bytearray()
    while len(out) < length:
        b = src[pos]; pos += 1
        if b & 0x80:
            if b & 0x40:                       # run of one byte
                k = (b & 0x1F) + 2
                if b & 0x20:
                    k += src[pos] << 5; pos += 1
                value = src[pos]; pos += 1
                out.extend(bytes([value]) * min(k, length - len(out)))
            else:                              # copy from earlier output
                offset = ((b & 0x03) << 8) + src[pos] + 1; pos += 1
                count = ((b >> 2) & 0x0F) + 2
                start = len(out) - offset
                for n in range(count):
                    if len(out) >= length:
                        break
                    out.append(out[start + n])
        else:
            if b & 0x40:                       # repeated block of literal bytes
                count = (b & 0x3F) + 2
                k = src[pos] + 1; pos += 1
                block = src[pos:pos + count]; pos += count
                for _ in range(k):
                    out.extend(block)
                del out[length:]
            else:                              # literal run
                count = (b & 0x1F) + 1
                if b & 0x20:
                    count += src[pos] << 5; pos += 1
                take = min(count, length - len(out))
                out.extend(src[pos:pos + take]); pos += count
    return bytes(out)


def lnk_entries(path):
    """[(name, offset, length)] of a KID LNK archive."""
    with open(path, "rb") as f:
        head = f.read(16)
        magic, count = struct.unpack_from("<II", head)
        if magic != 0x004B4E4C:
            raise ValueError(f"{path}: not a LNK archive ({magic:08x})")
        table = f.read(32 * count)
        size = os.path.getsize(path)
    entries = []
    for n in range(count):
        raw_offset, _ = struct.unpack_from("<II", table, 32 * n)
        name = table[32 * n + 8:32 * n + 32].split(b"\0", 1)[0].decode("ascii").lower()
        entries.append([name, 16 + 32 * count + raw_offset, 0])
    for n in range(count):
        end = entries[n + 1][1] if n + 1 < count else size
        entries[n][2] = end - entries[n][1]
    return [tuple(e) for e in entries]


def lnk_read(archive_file, offset, length):
    """Bytes of one archive member (LND-compressed members are decompressed)."""
    archive_file.seek(offset)
    data = archive_file.read(length)
    if len(data) >= 16 and struct.unpack_from("<I", data)[0] == 0x00646E6C:   # 'lnd\0'
        ulen = struct.unpack_from("<I", data, 8)[0]
        return lnd_decompress(data, 16, ulen)
    return data


def lnk_extract(path, out_dir, only_ext=None):
    """Extracts every member (optionally only names ending in `only_ext`); returns the count."""
    os.makedirs(out_dir, exist_ok=True)
    count = 0
    with open(path, "rb") as f:
        for name, offset, length in lnk_entries(path):
            if only_ext and not name.endswith(only_ext):
                continue
            with open(os.path.join(out_dir, name), "wb") as out:
                out.write(lnk_read(f, offset, length))
            count += 1
    return count


# ---------------------------------------------------------------- CPS / PRT images

def cps_to_prt(data):
    """Undoes CPS obfuscation and compression; returns the PRT image bytes."""
    buf = bytearray(data)
    cps_size = struct.unpack_from("<I", buf, 4)[0]
    flags = struct.unpack_from("<H", buf, 10)[0]
    prt_size = struct.unpack_from("<I", buf, 12)[0]
    last = struct.unpack_from("<I", buf, cps_size - 4)[0]
    if last != 0x07534682:
        edi = (last - 0x07534682) & 0xFFFFFFFF
        if edi != 0:
            ecx = (struct.unpack_from("<I", buf, edi)[0] + edi + 0x3786425) & 0xFFFFFFFF
            pos = 0x10
            while pos < cps_size - 4:
                if pos != edi:
                    value = struct.unpack_from("<I", buf, pos)[0]
                    struct.pack_into("<I", buf, pos, (value - (cps_size + ecx)) & 0xFFFFFFFF)
                ecx = (ecx * 0x41C64E6D + 0x9B06) & 0xFFFFFFFF
                pos += 4
            struct.pack_into("<I", buf, cps_size - 4, 0)
    if flags & 3:
        # The Java reads 20 header bytes (magic, size, two shorts, prtSize, prtMagic) before
        # decompressing, so the compressed stream starts at offset 0x14.
        return lnd_decompress(bytes(buf), 0x14, prt_size)
    # Uncompressed: the PRT image also starts after the 20-byte header (0x10 holds a
    # 4-byte tag such as "bmp\0"), as in the Java reader.
    return bytes(buf[0x14:])


def prt_to_image(prt, default_size=(800, 600)):
    """PRT image bytes -> (PIL RGBA image, (left, top) offset). A size of 0 means full screen."""
    import numpy as np
    from PIL import Image
    if struct.unpack_from("<I", prt, 0)[0] != 0x00545250:
        raise ValueError("not a PRT image")
    version = struct.unpack_from("<H", prt, 4)[0]
    pos = 6 + 5
    palette_bytes = prt[pos]; pos += 1
    left = top = 0
    if version == 0x65:
        w, h = struct.unpack_from("<HH", prt, pos); pos += 4
        has_alpha = struct.unpack_from("<I", prt, pos)[0] != 0; pos += 4
    else:
        pos += 4
        has_alpha = struct.unpack_from("<I", prt, pos)[0] != 0; pos += 4
        left, top, w, h = struct.unpack_from("<iiii", prt, pos); pos += 16
    if w <= 0:
        w = default_size[0]
    if h <= 0:
        h = default_size[1]
    if w >= 2048 or h >= 2048:
        raise ValueError(f"bad PRT size {w}x{h}")
    palette = None
    if palette_bytes == 4:
        palette = np.frombuffer(prt, dtype="<u4", count=256, offset=pos).copy(); pos += 1024
    bpp = 1 if palette_bytes > 0 else 3
    remaining = len(prt) - pos - (w * h if has_alpha and palette_bytes <= 0 else 0)
    row = w * bpp + max(0, (remaining - w * h * bpp) // h)          # rows are padded
    rows = np.frombuffer(prt, dtype=np.uint8, count=row * h, offset=pos).reshape(h, row)[::-1]
    pos += row * h
    if palette is not None:
        argb = palette[rows[:, :w]]
        rgba = np.stack([(argb >> 16) & 255, (argb >> 8) & 255, argb & 255,
                         (argb >> 24) & 255 if has_alpha else np.full_like(argb, 255)], axis=-1).astype(np.uint8)
    else:
        bgr = rows[:, :w * 3].reshape(h, w, 3)
        rgba = np.empty((h, w, 4), np.uint8)
        rgba[..., 0], rgba[..., 1], rgba[..., 2] = bgr[..., 2], bgr[..., 1], bgr[..., 0]
        rgba[..., 3] = 255
        if has_alpha:
            rgba[..., 3] = np.frombuffer(prt, dtype=np.uint8, count=w * h, offset=pos).reshape(h, w)
    return Image.fromarray(rgba, "RGBA"), (left, top)


def cps_to_image(data, default_size=(800, 600)):
    return prt_to_image(cps_to_prt(data), default_size)


# ---------------------------------------------------------------- WAF audio

def waf_to_wav(data):
    """KID 'WAF' audio (MS-ADPCM with a short header) -> standard WAV bytes."""
    if struct.unpack_from("<I", data, 0)[0] != 0x00464157:      # 'WAF\0'
        raise ValueError("not a WAF file")
    feature = bytearray(data[4:20])
    feature[0] = 2                                              # WAVE_FORMAT_ADPCM
    coefficients = data[20:52]
    size = struct.unpack_from("<I", data, 52)[0]
    body = data[56:56 + size]
    header = (b"RIFF" + struct.pack("<I", 82 + size) + b"WAVEfmt " + struct.pack("<I", 50)
              + bytes(feature) + struct.pack("<H", len(coefficients)) + coefficients
              + b"fact" + struct.pack("<I", 4) + bytes([0x30, 0x83, 0x6A, 0x00])
              + b"data" + struct.pack("<I", size))
    return header + body
