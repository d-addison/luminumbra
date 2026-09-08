"""Compare GLB exact bytes and scoped document/buffer identity independently."""
from pathlib import Path

from common import canonical, digest, module


def compare(first, repeat):
    parser = module("pinned_glb_parser", "tools/blender/validate_glb.py")
    a, binary_a = parser.parse_glb(Path(first))
    b, binary_b = parser.parse_glb(Path(repeat))
    binary_a = binary_a[:a["buffers"][0]["byteLength"]]
    binary_b = binary_b[:b["buffers"][0]["byteLength"]]
    return {"exact_bytes_equal": Path(first).read_bytes() == Path(repeat).read_bytes(),
            "canonical_document_and_buffer_equal": canonical(a) == canonical(b) and binary_a == binary_b,
            "first_canonical_sha256": digest(canonical(a) + binary_a),
            "repeat_canonical_sha256": digest(canonical(b) + binary_b),
            "scope": "Canonical JSON and declared binary bytes; accessor reordering and cross-platform float tolerance are not normalized"}
