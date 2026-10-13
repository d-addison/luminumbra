"""Bounded loopback-only HTTP broker helpers; no adapters or model processes.

The built-in capability fixture is the sole P1 executable backend card. Model
cards can be validated and gated for planning, but model execution is disabled.
"""
from copy import deepcopy
import base64
import http.client
import math
import os
import re
import struct
import time
import zlib

from .contracts import (Refusal, canonical, digest, keys, parse_json, read_bounded,
                        relative_path, require)
from .meshgen_config import inert_json
from .meshgen_contracts import BACKEND, validate_backend_params
from .meshgen_licence import CLAUSES
from .meshgen_registry import (BOOL, ID_SCHEMA, SHA_SCHEMA, array, enum, number,
                               obj, string, validate_schema)

MIB = 1024 ** 2
MAX_IMAGE = 16 * MIB
MAX_UPLOAD = 4 * ((MAX_IMAGE + 2) // 3) + MIB
MAX_JSON = MIB
MAX_RESULT = 256 * MIB
JOB_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")
LICENCE_SCHEMA = obj({"id": string(), "text_sha256": SHA_SCHEMA, "url": string(),
    "clauses": array(enum(*CLAUSES), 32), "executed": BOOL}, ("id", "text_sha256", "clauses"))
COMPONENT_SCHEMA = obj({"role": string(), "name": string(), "repo": string(),
    "revision": string(r"[0-9a-f]{40}", 40), "weights_sha256": array(SHA_SCHEMA, 256),
    "precision": string(), "licence": LICENCE_SCHEMA}, ("role", "name"))
CAPABILITY_SCHEMA = obj({
    "contract": {"type": "string", "const": BACKEND},
    "backend": obj({"id": ID_SCHEMA, "version": string(), "adapter_commit": string(r"[0-9a-f]{40}", 40),
                    "protocol": {"type": "integer", "const": 1}}, ("id", "version", "adapter_commit", "protocol")),
    "components": array(COMPONENT_SCHEMA, 256),
    "dataset_chain": obj({"audited": BOOL, "notes": string(), "clauses": array(enum(*CLAUSES), 32),
                          "licences": array(LICENCE_SCHEMA, 256)}, ("audited",)),
    "params_schema": obj(dynamic={}, maximum=128),
    "inputs": obj({"image": obj({"formats": array(enum("png", "jpeg"), 2, 1), "alpha": enum("optional", "required", "none"),
                        "max_bytes": number(1, MAX_IMAGE, True)}, ("formats", "alpha", "max_bytes")),
                   "prompt": obj({"supported": BOOL}, ("supported",))}, ("image", "prompt")),
    "outputs": obj({"format": enum("glb"), "mesh": enum("triangles"), "topology": string(),
        "has_uv": BOOL, "has_normals": BOOL, "texture": enum("none", "embedded", "separate"),
        "units": enum("metres", "arbitrary_normalised"), "up_axis": enum("Y"),
        "max_triangles_hint": number(1, 4000000, True)},
        ("format", "mesh", "topology", "has_uv", "has_normals", "texture", "units", "up_axis", "max_triangles_hint")),
    "service_side_decimation": obj({"supported": BOOL, "param": string()}, ("supported",)),
    "resources": obj({"vram_peak_mib_estimate": {"oneOf": [number(0, 2**20), {"type": "null"}]},
                      "estimate_source": string(), "cpu_threads": number(1, 256, True)},
                     ("vram_peak_mib_estimate", "estimate_source", "cpu_threads")),
    "determinism": obj({"seeded": BOOL, "bitwise_reproducible": BOOL, "notes": string()},
                       ("seeded", "bitwise_reproducible", "notes")),
    "limits": obj({"job_deadline_s_default": number(0.01, 1800), "job_deadline_s_max": number(0.01, 1800)},
                  ("job_deadline_s_default", "job_deadline_s_max"))},
    ("contract", "backend", "components", "dataset_chain", "params_schema", "inputs", "outputs",
     "service_side_decimation", "resources", "determinism", "limits"))

# No executable code, datasets, GPU or weights are involved in this fixture.
MOCK_CAPABILITY = {
    "contract": BACKEND,
    "backend": {"id": "mock", "version": "1", "adapter_commit": "0" * 40, "protocol": 1},
    "components": [{"role": "shape", "name": "synthetic-stdlib", "revision": "0" * 40,
        "weights_sha256": [], "licence": {"id": "CC0-1.0", "text_sha256": digest(b"Luminumbra synthetic mock fixture: CC0-1.0"), "clauses": []}}],
    "dataset_chain": {"audited": True, "notes": "No training data; deterministic synthetic geometry."},
    "params_schema": {"shape": {"type": "string", "enum": ["cube", "icosphere"]}},
    "inputs": {"image": {"formats": ["png", "jpeg"], "alpha": "optional", "max_bytes": MAX_IMAGE},
               "prompt": {"supported": False}},
    "outputs": {"format": "glb", "mesh": "triangles", "topology": "synthetic", "has_uv": False,
        "has_normals": False, "texture": "none", "units": "metres", "up_axis": "Y", "max_triangles_hint": 20},
    "service_side_decimation": {"supported": False},
    "resources": {"vram_peak_mib_estimate": None, "estimate_source": "unavailable", "cpu_threads": 1},
    "determinism": {"seeded": True, "bitwise_reproducible": True, "notes": "Synthetic fixture; no inference."},
    "limits": {"job_deadline_s_default": 60, "job_deadline_s_max": 1800}}
MOCK_CAPABILITY_DIGEST = digest(canonical(MOCK_CAPABILITY))


def bounded_json(data, *, depth=32, items=65536, max_bytes=MAX_JSON):
    require(len(data) <= max_bytes, "backend.limit", "JSON exceeds its byte limit.")
    # Scan before recursive decoding; strings and escapes cannot affect depth.
    nesting, quoted, escaped = 0, False, False
    for byte in data:
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (91, 123):
            nesting += 1
            require(nesting <= depth, "backend.limit", "JSON nesting exceeds its limit.")
        elif byte in (93, 125):
            nesting -= 1
    try:
        value = parse_json(data)
        inert_json(value, max_depth=depth, max_items=items)
        return value
    except (ValueError, TypeError, RecursionError) as error:
        if isinstance(error, Refusal):
            raise
        raise Refusal("backend.protocol", "Malformed backend JSON.") from error


def validate_capability(card, backend_id, config):
    validate_schema(card, CAPABILITY_SCHEMA)
    inert_json(card)
    from .meshgen_registry import check_schema
    schema = card["params_schema"]
    check_schema(schema if "type" in schema or "oneOf" in schema else obj(schema))
    require(card["backend"]["id"] == backend_id, "backend.pin_mismatch", "Backend identity differs from the project pin.")
    identity = digest(canonical(card))
    if config["kind"] == "mock":
        require(backend_id == "mock" and identity == MOCK_CAPABILITY_DIGEST,
                "backend.pin_mismatch", "Mock capability must match the complete built-in fixture.")
    else:
        pins = config.get("pins", {})
        require(bool(pins), "config.pins_missing", "Model backend has no committed pins.")
        shape = [item for item in card["components"] if item["role"] == "shape"]
        require(len(shape) == 1 and shape[0].get("revision") == pins["model_revision"]
                and shape[0].get("weights_sha256") == pins["weights_sha256"],
                "backend.pin_mismatch", "Model revision or weight hashes differ from committed pins.")
    require(card["limits"]["job_deadline_s_default"] <= card["limits"]["job_deadline_s_max"],
            "backend.protocol", "Backend default deadline exceeds its maximum.")
    return identity


def job_id(value):
    require(isinstance(value, str) and JOB_ID.fullmatch(value), "backend.protocol", "Malformed backend job identifier.")
    return value


def token_from_source(project, source, minimum=32):
    if "env" in source:
        token = os.environ.get(source["env"], "")
    else:
        token = read_bounded(relative_path(project, source["file"]), 4096).decode("ascii").strip()
    require(isinstance(token, str) and minimum <= len(token) <= 4096 and token.isascii()
            and all(33 <= ord(char) <= 126 for char in token),
            "session.token", "Backend token must be a bounded printable bearer token.")
    return token


class BackendClient:
    """No proxy environment, DNS aliases, redirects, chunking or unbounded reads."""
    def __init__(self, endpoint, token, *, connect_s=5, clock=time.monotonic,
                 connection_factory=http.client.HTTPConnection, json_limit=MAX_JSON,
                 json_depth=32, collection_items=65536):
        match = re.fullmatch(r"http://127\.0\.0\.1:([0-9]{1,5})", endpoint)
        require(match is not None and 1 <= int(match[1]) <= 65535,
                "backend.loopback", "Use literal http://127.0.0.1:<port>.")
        require(isinstance(token, str) and 32 <= len(token) <= 4096 and token.isascii()
                and all(33 <= ord(char) <= 126 for char in token), "session.token", "Invalid backend bearer token.")
        require(type(connect_s) in (int, float) and math.isfinite(connect_s) and 0 < connect_s <= 30,
                "request.invalid", "Invalid connection timeout.")
        require(all(type(value) is int and 1 <= value <= maximum for value, maximum in
                    ((json_limit, MAX_JSON), (json_depth, 32), (collection_items, 65536))),
                "request.limit", "Invalid JSON parser limits.")
        self.port, self.token, self.connect_s = int(match[1]), token, connect_s
        self.clock, self.factory = clock, connection_factory
        self.json_limit, self.json_depth, self.collection_items = json_limit, json_depth, collection_items

    def request(self, method, path, *, body=None, limit=None, deadline=None, binary=False):
        limit = self.json_limit if limit is None else limit
        match = re.fullmatch(r"/v1/jobs/[A-Za-z0-9][A-Za-z0-9_-]{0,63}(/cancel|/result)?", path)
        require((method, path) in (("GET", "/v1/capabilities"), ("POST", "/v1/jobs"))
                or (match and ((method == "POST" and match[1] == "/cancel")
                    or (method == "GET" and match[1] in (None, "/result")))),
                "backend.protocol", "Unsupported backend endpoint.")
        require(type(limit) is int and 0 < limit <= (MAX_RESULT if binary else MAX_JSON),
                "request.limit", "Invalid response limit.")
        payload = None if body is None else canonical(body)
        require(payload is None or len(payload) <= MAX_UPLOAD, "request.limit", "Backend upload exceeds its bound.")
        deadline = self.clock() + self.connect_s if deadline is None else deadline
        require(type(deadline) in (int, float) and math.isfinite(deadline), "request.invalid", "Invalid absolute deadline.")

        def remaining():
            value = deadline - self.clock()
            require(value > 0, "backend.deadline", "Backend deadline expired.")
            return min(self.connect_s, value)

        connection = self.factory("127.0.0.1", self.port, timeout=remaining(), source_address=("127.0.0.1", 0))
        try:
            connection.connect()
            require(connection.sock.getpeername()[0] == "127.0.0.1", "backend.loopback", "Runtime peer is not the pinned loopback address.")
            connection.sock.settimeout(remaining())
            peer_socket = connection.sock
            connection.sock = DeadlineSocket(peer_socket, remaining)
            connection.request(method, path, body=payload, headers={
                "Host": f"127.0.0.1:{self.port}", "Authorization": "Bearer " + self.token,
                "Content-Type": "application/json", "Connection": "close"})
            peer_socket.settimeout(remaining())
            # http.client's header parser otherwise has only a relative socket
            # timeout. This reader checks the absolute clock on every refill,
            # including headers, so a drip-fed line cannot extend the deadline.
            connection.sock = DeadlineSocket(peer_socket, remaining)
            response = connection.getresponse()
            require(not 300 <= response.status < 400, "backend.redirect", "Backend redirects are refused.")
            require(response.status == 200, "backend.unavailable", "Backend refused the authenticated request.")
            lengths = response.headers.get_all("Content-Length", [])
            require(len(lengths) == 1 and re.fullmatch(r"[0-9]{1,12}", lengths[0])
                    and not response.headers.get_all("Transfer-Encoding"),
                    "backend.framing", "Require one finite Content-Length and no Transfer-Encoding.")
            size = int(lengths[0])
            require(0 < size <= limit, "backend.limit", "Backend response exceeds its bound.")
            expected = "model/gltf-binary" if binary else "application/json"
            require(response.headers.get_all("Content-Type", []) == [expected],
                    "backend.protocol", "Unexpected response content type.")
            chunks, received = [], 0
            while received < size:
                # Reapply the absolute deadline for every streaming read; a slow
                # trickle cannot extend the job by resetting a relative timer.
                peer_socket.settimeout(remaining())
                chunk = response.read1(min(65536, size - received))
                require(bool(chunk), "backend.framing", "Backend body ended before Content-Length.")
                received += len(chunk)
                chunks.append(chunk)
            remaining()
            data = b"".join(chunks)
            return data if binary else bounded_json(data, depth=self.json_depth, items=self.collection_items,
                                                   max_bytes=self.json_limit)
        except (OSError, http.client.HTTPException) as error:
            rule = "backend.deadline" if self.clock() >= deadline else "backend.unavailable"
            raise Refusal(rule, "Backend connection failed or timed out.") from error
        finally:
            connection.close()

    def capabilities(self, **kwargs):
        return self.request("GET", "/v1/capabilities", **kwargs)

    def submit(self, body, **kwargs):
        value = self.request("POST", "/v1/jobs", body=body, **kwargs)
        keys(value, ("job_id",))
        return job_id(value["job_id"])

    def status(self, identifier, **kwargs):
        value = self.request("GET", "/v1/jobs/" + job_id(identifier), **kwargs)
        keys(value, ("job_id", "state"), ("progress", "result", "errors"))
        require(value["job_id"] == identifier and value["state"] in ("queued", "running", "succeeded", "failed", "cancelled"),
                "backend.protocol", "Invalid backend status.")
        if "progress" in value:
            validate_schema(value["progress"], number(0, 1))
        if "errors" in value:
            validate_schema(value["errors"], array(string(maximum=4096), 64))
        if value["state"] == "succeeded":
            validate_result(value.get("result"))
        else:
            require("result" not in value, "backend.protocol", "Nonterminal backend returned result metadata.")
        return value

    def cancel(self, identifier, **kwargs):
        value = self.request("POST", "/v1/jobs/" + job_id(identifier) + "/cancel", body={}, **kwargs)
        keys(value, ("job_id", "state"))
        require(value["job_id"] == identifier and value["state"] == "cancelled", "backend.protocol", "Invalid cancellation response.")
        return value

    def result(self, identifier, **kwargs):
        return self.request("GET", "/v1/jobs/" + job_id(identifier) + "/result", binary=True, limit=kwargs.pop("limit", MAX_RESULT), **kwargs)


class DeadlineSocket:
    """Socket facade used only to give HTTPResponse a bounded timed reader."""
    def __init__(self, sock, remaining):
        self.sock, self.remaining = sock, remaining

    def __getattr__(self, name):
        return getattr(self.sock, name)

    def sendall(self, data, flags=0):
        view = memoryview(data)
        for offset in range(0, len(view), 65536):
            self.sock.settimeout(self.remaining())
            self.sock.sendall(view[offset:offset+65536], flags)
            self.remaining()

    def makefile(self, mode, *args):
        return DeadlineReader(self.sock.makefile(mode, *args), self.sock, self.remaining)


class DeadlineReader:
    def __init__(self, stream, sock, remaining):
        self.stream, self.sock, self.remaining = stream, sock, remaining
        self.buffer, self.header_bytes = bytearray(), 0

    def refill(self, size):
        self.sock.settimeout(self.remaining())
        chunk = self.stream.read1(size)
        self.remaining()
        return chunk

    def readline(self, limit=-1):
        limit = 65537 if limit < 0 else min(limit, 65537)
        while b"\n" not in self.buffer and len(self.buffer) < limit:
            chunk = self.refill(min(4096, limit-len(self.buffer)))
            if not chunk:
                break
            self.buffer.extend(chunk)
        end = self.buffer.find(b"\n")
        size = min(limit, end+1 if end >= 0 else len(self.buffer))
        line = bytes(self.buffer[:size])
        del self.buffer[:size]
        self.header_bytes += size
        require(self.header_bytes <= 65536, "backend.limit", "HTTP headers exceed 64 KiB.")
        return line

    def read1(self, size=-1):
        require(0 < size <= 65536, "backend.limit", "HTTP reads must be bounded.")
        self.remaining()
        if self.buffer:
            data = bytes(self.buffer[:size])
            del self.buffer[:size]
            return data
        return self.refill(size)

    def read(self, size=-1):
        require(0 <= size <= 65536, "backend.limit", "HTTP reads must be bounded.")
        result = bytearray()
        while len(result) < size:
            chunk = self.read1(size-len(result))
            if not chunk:
                break
            result.extend(chunk)
        return bytes(result)

    def close(self):
        self.stream.close()


def validate_result(value):
    validate_schema(value, obj({"sha256": SHA_SCHEMA, "bytes": number(1, MAX_RESULT, True),
        "backend_reported": obj({"seed": number(0, 2**32-1, True), "steps_run": number(0, 100000, True),
            "wall_s": number(0, 1800), "peak_vram_mib": {"oneOf": [number(0, 2**20), {"type": "null"}]}},
            ("seed", "steps_run", "wall_s", "peak_vram_mib")),
        "logs_tail": string(maximum=16384)}, ("sha256", "bytes", "backend_reported", "logs_tail")))
    require(len(value["logs_tail"].encode("utf-8")) <= 16384, "backend.limit", "Backend log exceeds 16 KiB.")
    return value


def jpeg_scan_layout(scan, components, dimensions):
    require(sum(item[1]*item[2] for item in components.values()) <= 10,
            "request.invalid", "Unsupported JPEG sampling expansion.")
    max_h = max(item[1] for item in components.values())
    max_v = max(item[2] for item in components.values())
    if len(scan) == 1:
        identity, dc, ac = scan[0]
        _, h, v = components[identity]
        columns = (dimensions[0]*h + 8*max_h-1) // (8*max_h)
        rows = (dimensions[1]*v + 8*max_v-1) // (8*max_v)
        blocks = [(dc, ac)]
    else:
        columns = (dimensions[0]+8*max_h-1) // (8*max_h)
        rows = (dimensions[1]+8*max_v-1) // (8*max_v)
        blocks = [(dc, ac) for identity, dc, ac in scan
                  for _ in range(components[identity][1]*components[identity][2])]
    return columns*rows, blocks


def validate_jpeg_entropy(parts, restarts, scan, components, dimensions, tables, interval):
    """Check sequential Huffman scan grammar without allocating decoded pixels.

    DCT values are irrelevant to credential intake, but the exact number of
    blocks and coefficient symbols must exist; an EOI after a few bytes cannot
    masquerade as a large complete image.
    """
    mcus, blocks = jpeg_scan_layout(scan, components, dimensions)
    expected_parts = (mcus+interval-1)//interval if interval else 1
    require(len(parts) == expected_parts and len(restarts) == expected_parts-1
            and all(marker == i % 8 for i, marker in enumerate(restarts)),
            "request.invalid", "JPEG restart sequence is incomplete or misplaced.")
    processed = 0
    for part in parts:
        bit = 0
        def take(width):
            nonlocal bit
            require(bit+width <= len(part)*8, "request.invalid", "Truncated JPEG entropy data.")
            value = 0
            for _ in range(width):
                value = (value << 1) | ((part[bit//8] >> (7-bit%8)) & 1)
                bit += 1
            return value
        def symbol(table):
            code = 0
            for width in range(1, 17):
                code = (code << 1) | take(1)
                if (width, code) in table:
                    return table[width, code]
            raise Refusal("request.invalid", "Invalid JPEG Huffman code.")
        count = min(interval or mcus, mcus-processed)
        for _ in range(count):
            for dc, ac in blocks:
                take(symbol(tables[0, dc]))
                coefficient = 1
                while coefficient < 64:
                    value = symbol(tables[1, ac])
                    run, width = value >> 4, value & 15
                    if value == 0:
                        break
                    if value == 0xf0:
                        coefficient += 16
                        require(coefficient <= 64, "request.invalid", "JPEG zero run escapes its block.")
                    else:
                        coefficient += run
                        require(coefficient < 64, "request.invalid", "JPEG coefficient escapes its block.")
                        take(width)
                        coefficient += 1
            processed += 1
        padding = len(part)*8-bit
        require(0 <= padding <= 7 and take(padding) == (1 << padding)-1,
                "request.invalid", "JPEG entropy has extra bytes or invalid padding.")


def image_snapshot(project, descriptor, limits, provenance_policy):
    """Hash one retained byte snapshot; refuse credentials we cannot evaluate.

    Supports uncompressed PNG tEXt JSON CAWG assertions. JUMBF/C2PA, compressed
    text and XMP credential containers fail closed, rather than guessing rights.
    No image waiver exists in P1.
    """
    path = relative_path(project, descriptor["path"])
    data = read_bounded(path, min(limits["image_bytes"], MAX_IMAGE))
    require(digest(data) == descriptor["sha256"], "request.conflict", "Reference image changed from its declared digest.")
    metadata, metadata_size, credentials, dimensions = [], 0, False, None
    def check_dimensions():
        require(dimensions and all(0 < part <= limits["image_dimension"] for part in dimensions)
                and dimensions[0]*dimensions[1] <= limits["image_pixels"], "request.limit", "Image dimensions exceed bounds.")

    def note(payload):
        nonlocal metadata_size
        metadata_size += len(payload)
        metadata.append(payload)
        require(metadata_size <= limits["metadata_bytes"], "request.limit", "Image metadata exceeds bounds.")

    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        offset, count, ended, idat_ended = 8, 0, False, False
        compressed, palette, colour = [], False, None
        while offset < len(data):
            require(offset + 12 <= len(data), "request.invalid", "Truncated PNG chunk.")
            size = struct.unpack_from(">I", data, offset)[0]
            kind = data[offset+4:offset+8]
            require(size <= len(data)-offset-12 and count < limits["collection_items"], "request.limit", "PNG chunk limits exceeded.")
            payload = data[offset+8:offset+8+size]
            crc = struct.unpack_from(">I", data, offset+8+size)[0]
            require(zlib.crc32(kind + payload) & 0xffffffff == crc, "request.invalid", "PNG CRC mismatch.")
            if count == 0:
                require(kind == b"IHDR" and size == 13, "request.invalid", "PNG requires an IHDR first.")
                width, height, bits, colour, compression, filtering, interlace = struct.unpack(">IIBBBBB", payload)
                dimensions = width, height
                check_dimensions()
                require(bits == 8 and colour in (0, 2, 4, 6) and (compression, filtering, interlace) == (0, 0, 0),
                        "request.invalid", "Unsupported PNG pixel layout.")
            elif kind == b"PLTE":
                require(not palette and not compressed and colour in (2, 3, 6) and 0 < size <= 768 and size % 3 == 0,
                        "request.invalid", "Invalid PNG palette layout.")
                palette = True
            elif kind == b"IDAT":
                require(not idat_ended and (colour != 3 or palette), "request.invalid", "Invalid PNG image data order.")
                compressed.append(payload)
            elif kind == b"IEND":
                require(size == 0 and offset+12 == len(data) and compressed, "request.invalid", "PNG requires image data and a final end marker.")
                ended = True
            elif kind in (b"tEXt", b"iTXt", b"zTXt", b"caBX", b"eXIf"):
                note(payload)
                if kind == b"tEXt" and payload.startswith(b"cawg.training-and-data-mining\0"):
                    credentials = True
                    assertions = bounded_json(payload.split(b"\0", 1)[1])
                    require(isinstance(assertions, dict) and bool(assertions), "licence.image_credentials", "Empty CAWG assertion.")
                    for name, assertion in assertions.items():
                        require(name in ("cawg.ai_training", "cawg.ai_inference", "cawg.data_mining") and assertion == "allowed",
                                "licence.image_reserved", "Reserved, constrained or unknown image rights are refused.")
                else:
                    require(kind == b"tEXt" and b"\0" in payload and 1 <= len(payload.split(b"\0", 1)[0]) <= 79
                            and not any(word in payload.lower() for word in (b"c2pa", b"cawg", b"jumbf", b"xmp")),
                            "licence.image_credentials", "Unsupported embedded credential format.")
            else:
                raise Refusal("request.invalid", "Unsupported PNG chunk layout.")
            if compressed and kind != b"IDAT":
                idat_ended = True
            offset += size + 12
            count += 1
            if ended:
                break
        require(ended, "request.invalid", "PNG has no end marker.")
        # Validate the complete bounded zlib stream and every scanline filter.
        row = dimensions[0] * {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour] + 1
        expected, produced = row * dimensions[1], 0
        decoder = zlib.decompressobj()
        try:
            for piece in compressed:
                # zlib copies unconsumed_tail. Keep both input and that tail
                # bounded, even for one large, poorly compressible IDAT.
                for at in range(0, len(piece), 65536):
                    pending = memoryview(piece)[at:at+65536]
                    while pending:
                        output = decoder.decompress(pending, min(65536, expected-produced+1))
                        require(produced+len(output) <= expected, "request.invalid", "PNG pixel expansion exceeds its layout.")
                        for position in range((-produced) % row, len(output), row):
                            require(output[position] <= 4, "request.invalid", "Invalid PNG scanline filter.")
                        produced += len(output)
                        pending = decoder.unconsumed_tail
                        require(not decoder.unused_data, "request.invalid", "PNG has trailing compressed data.")
            require(decoder.eof and produced == expected, "request.invalid", "PNG pixels are truncated or absent.")
        except zlib.error as error:
            raise Refusal("request.invalid", "Malformed PNG pixel stream.") from error
        image_format = "png"
    elif data.startswith(b"\xff\xd8"):
        offset, count, ended, in_scan, scan_bytes = 2, 0, False, False, 0
        components, scanned, quant, huffman = {}, set(), set(), {}
        restart_interval, scan = 0, []
        entropy_parts, restarts = [], []
        while offset < len(data):
            if in_scan:
                start = offset
                while offset < len(data):
                    if data[offset] != 255:
                        entropy_parts[-1].append(data[offset])
                        scan_bytes += 1
                        offset += 1
                        continue
                    require(offset+1 < len(data), "request.invalid", "Truncated JPEG scan.")
                    marker = data[offset+1]
                    if marker == 0:
                        entropy_parts[-1].append(255)
                        scan_bytes += 1
                        offset += 2
                    elif 0xd0 <= marker <= 0xd7:
                        require(restart_interval > 0, "request.invalid", "JPEG restart without an interval.")
                        require(len(entropy_parts) < expected_parts and marker-0xd0 == len(restarts) % 8,
                                "request.invalid", "JPEG restart exceeds its MCU extent or sequence.")
                        count += 1
                        require(count <= limits["collection_items"], "request.limit", "JPEG segment limits exceeded.")
                        restarts.append(marker-0xd0)
                        entropy_parts.append(bytearray())
                        offset += 2
                    elif marker == 255:
                        offset += 1
                    else:
                        break
                require(scan_bytes > 0 and offset > start, "request.invalid", "JPEG scan contains no data.")
                validate_jpeg_entropy(entropy_parts, restarts, scan, components, dimensions, huffman, restart_interval)
                in_scan = False
            require(offset+2 <= len(data) and data[offset] == 255, "request.invalid", "Invalid JPEG marker.")
            while offset+1 < len(data) and data[offset+1] == 255:
                offset += 1
            require(offset+1 < len(data), "request.invalid", "Truncated JPEG marker fill.")
            marker = data[offset+1]
            count += 1
            require(count <= limits["collection_items"], "request.limit", "JPEG segment limits exceeded.")
            if marker == 0xd9:
                require(offset+2 == len(data) and components and scanned == set(components), "request.invalid", "JPEG has incomplete scans or trailing data.")
                ended = True
                break
            require(marker not in (0, 0xd8, 0x01) and not 0xd0 <= marker <= 0xd7 and offset+4 <= len(data),
                    "request.invalid", "Unsupported JPEG marker layout.")
            size = struct.unpack_from(">H", data, offset+2)[0]
            require(2 <= size <= len(data)-offset-2, "request.invalid", "Truncated JPEG segment.")
            payload = data[offset+4:offset+2+size]
            if 0xe0 <= marker <= 0xef or marker == 0xfe:
                note(payload)
                # EXIF/TIFF and vendor APP containers can hide credentials; only
                # the fixed JFIF envelope and plain comments are supported.
                require((marker == 0xe0 and payload.startswith(b"JFIF\0") and len(payload) >= 14
                         and len(payload) == 14 + 3*payload[12]*payload[13]) or marker == 0xfe,
                        "licence.image_credentials", "Unsupported JPEG metadata container.")
                require(not any(word in payload.lower() for word in (b"c2pa", b"cawg", b"jumbf", b"xmp")),
                        "licence.image_credentials", "Unsupported JPEG credential container.")
            elif marker in (0xc0, 0xc1):
                require(not components and len(payload) >= 6 and payload[0] == 8 and payload[5] in (1, 3)
                        and len(payload) == 6+3*payload[5], "request.invalid", "Unsupported JPEG frame.")
                height, width = struct.unpack_from(">HH", payload, 1)
                dimensions = width, height
                check_dimensions()
                for i in range(payload[5]):
                    identity, sampling, table = payload[6+i*3:9+i*3]
                    require(identity not in components and 1 <= sampling >> 4 <= 4 and 1 <= sampling & 15 <= 4
                            and table <= 3, "request.invalid", "Invalid JPEG frame component.")
                    components[identity] = (table, sampling >> 4, sampling & 15)
            elif marker == 0xdb:
                cursor = 0
                while cursor < len(payload):
                    table = payload[cursor]
                    require(table >> 4 == 0 and table & 15 <= 3 and cursor+65 <= len(payload)
                            and all(payload[cursor+1:cursor+65]), "request.invalid", "Invalid JPEG quantization table.")
                    quant.add(table & 15)
                    cursor += 65
                require(cursor > 0, "request.invalid", "Empty JPEG quantization tables.")
            elif marker == 0xc4:
                cursor = 0
                while cursor < len(payload):
                    require(cursor+17 <= len(payload), "request.invalid", "Truncated JPEG Huffman table.")
                    table = payload[cursor]
                    counts = payload[cursor+1:cursor+17]
                    symbols = sum(counts)
                    slots = 1
                    for n in counts:
                        slots = slots*2-n
                        require(slots >= 0, "request.invalid", "Oversubscribed JPEG Huffman table.")
                    require(table >> 4 in (0, 1) and table & 15 <= 3 and 0 < symbols <= 256
                            and cursor+17+symbols <= len(payload), "request.invalid", "Invalid JPEG Huffman table.")
                    values = payload[cursor+17:cursor+17+symbols]
                    require(all(value <= 11 if table >> 4 == 0 else
                                value in (0, 0xf0) or 1 <= value & 15 <= 10 for value in values),
                            "request.invalid", "Unsupported JPEG coefficient symbols.")
                    lookup, code, at = {}, 0, 0
                    for width, n in enumerate(counts, 1):
                        for _ in range(n):
                            require(code < (1 << width)-1, "request.invalid", "JPEG Huffman code uses reserved all-one padding.")
                            lookup[width, code] = values[at]
                            at += 1
                            code += 1
                        code <<= 1
                    huffman[table >> 4, table & 15] = lookup
                    cursor += 17+symbols
                require(cursor > 0, "request.invalid", "Empty JPEG Huffman tables.")
            elif marker == 0xdd:
                require(len(payload) == 2, "request.invalid", "Invalid JPEG restart interval.")
                restart_interval = struct.unpack(">H", payload)[0]
            elif marker == 0xda:
                require(components and payload and 1 <= payload[0] <= len(components)
                        and len(payload) == 4+2*payload[0] and payload[-3:] == b"\0\x3f\0",
                        "request.invalid", "Unsupported JPEG scan layout.")
                scan = []
                for i in range(payload[0]):
                    identity, tables = payload[1+i*2:3+i*2]
                    require(identity in components and identity not in scanned and components[identity][0] in quant
                            and (0, tables >> 4) in huffman and (1, tables & 15) in huffman,
                            "request.invalid", "Invalid JPEG scan component or missing tables.")
                    scanned.add(identity)
                    scan.append((identity, tables >> 4, tables & 15))
                mcus, _ = jpeg_scan_layout(scan, components, dimensions)
                expected_parts = (mcus+restart_interval-1)//restart_interval if restart_interval else 1
                require(expected_parts <= limits["collection_items"], "request.limit", "JPEG restart collection exceeds bounds.")
                entropy_parts, restarts = [bytearray()], []
                in_scan, scan_bytes = True, 0
            else:
                raise Refusal("request.invalid", "Unsupported JPEG encoding or marker.")
            offset += size + 2
        require(ended, "request.invalid", "JPEG is truncated or contains no complete image scans.")
        image_format = "jpeg"
    else:
        raise Refusal("request.invalid", "Reference image must be PNG or JPEG.")
    require(dimensions and all(0 < part <= limits["image_dimension"] for part in dimensions)
            and dimensions[0]*dimensions[1] <= limits["image_pixels"], "request.limit", "Image dimensions exceed bounds.")
    require(sum(map(len, metadata)) <= limits["metadata_bytes"], "request.limit", "Image metadata exceeds bounds.")
    require(provenance_policy != "refuse_unknown" or credentials, "licence.image_unknown", "Absent credentials are not clearance under refuse_unknown policy.")
    return {"bytes": data, "sha256": digest(data), "format": image_format,
            "credentials": "evaluated" if credentials else "absent", "dimensions": list(dimensions)}


def submission(request, snapshot, params, deadline_s, max_tris_hint=None):
    body = {"contract": BACKEND, "request_id": request["request_id"], "image_ref": deepcopy(request["image_ref"]),
            "declared_source": request["declared_source"], "image_b64": base64.b64encode(snapshot["bytes"]).decode("ascii"),
            "seed": request["seed"], "params": deepcopy(params), "deadline_s": deadline_s}
    if "prompt" in request:
        body["prompt"] = request["prompt"]
    if max_tris_hint is not None:
        body["max_tris_hint"] = max_tris_hint
    return body


def validate_submission(value, card=MOCK_CAPABILITY):
    """Wire grammar used by the mock, separate from the small planning request."""
    from .meshgen_contracts import validate_request
    keys(value, ("contract", "request_id", "image_ref", "declared_source", "image_b64", "seed", "params", "deadline_s"),
         ("prompt", "max_tris_hint"))
    require(value["contract"] == BACKEND, "backend.protocol", "Unknown backend request contract.")
    planning = {key: value[key] for key in ("request_id", "image_ref", "declared_source", "seed", "params", "prompt") if key in value}
    validate_request({**planning, "kind": "backend", "backend_id": card["backend"]["id"], "asset_class": "default"})
    validate_backend_params(value["params"], card["params_schema"])
    validate_schema(value["deadline_s"], number(0.01, card["limits"]["job_deadline_s_max"]))
    if "max_tris_hint" in value:
        validate_schema(value["max_tris_hint"], number(64, 1000000, True))
    if "prompt" in value:
        require(card["inputs"]["prompt"]["supported"], "request.invalid", "Backend does not support prompts.")
    encoded = value["image_b64"]
    require(isinstance(encoded, str) and len(encoded) <= 4*((card["inputs"]["image"]["max_bytes"]+2)//3),
            "request.limit", "Encoded image exceeds its calculated allowance.")
    try:
        data = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError) as error:
        raise Refusal("request.invalid", "Invalid base64 image.") from error
    require(0 < len(data) <= card["inputs"]["image"]["max_bytes"] and digest(data) == value["image_ref"]["sha256"]
            and base64.b64encode(data).decode("ascii") == encoded, "request.invalid", "Image bytes, canonical encoding and digest must agree.")
    return data
