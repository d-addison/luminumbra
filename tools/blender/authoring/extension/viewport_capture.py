"""Explicit lossless export of one already validated viewport frame."""
import hashlib
import json
from pathlib import Path
import struct
import zlib


def capture(directory, header, payload):
    width, height = header['state']['width'], header['state']['height']
    pixels = width * height
    if (not 1 <= width <= 1280 or not 1 <= height <= 720 or len(payload) != pixels * 9 or
            hashlib.sha256(payload).hexdigest() != header['planes_sha256']):
        raise ValueError('Capture frame bounds or identity changed')
    directory = Path(directory)
    directory.mkdir(mode=0o700, parents=False, exist_ok=False)
    planes = {'rgba8.bin': payload[:4 * pixels], 'depth32f.bin': payload[4 * pixels:8 * pixels],
              'coverage8.bin': payload[8 * pixels:]}
    for name, value in planes.items():
        (directory / name).write_bytes(value)
    # OpenGL's first row is bottom-most. PNG stores top-most first; no color
    # transform or quantization is applied to the received display RGBA bytes.
    rows = b''.join(b'\0' + planes['rgba8.bin'][row * width * 4:(row + 1) * width * 4]
                    for row in range(height - 1, -1, -1))
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value))
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    (directory / 'color.png').write_bytes(png)
    receipt = {'schema': 'luminumbra.viewport.capture.v1', 'header': header,
               'visual_approved': False, 'blender_presentation_qualified': False,
               'planes_origin': 'bottom-left', 'png_origin': 'top-left',
               'files': {name: hashlib.sha256(value).hexdigest() for name, value in
                         {**planes, 'color.png': png}.items()}}
    (directory / 'frame.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt
