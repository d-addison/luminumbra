#!/usr/bin/env python3
"""Validate bounded diagnostic moving-anchor packets, without launching the engine.

Pressure proof and shipped-settings observations are separate profiles. Timings
include simulation only; mask/digest capture and save/load are reported separately.
This packet is diagnostic evidence, not a replacement for paired oracle artifacts.
Capture with regional_field_evidence (--diagnostic-pressure|--shipped-settings)
--out <packet.json>; production logs are separate from the packet file.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import stat
import sys

# Admission bounds, not simulation limits: 64 MiB, 640 frames, 256 owners and
# 1024 page masks per frame. Mask work is four words per physical page.
MAX_BYTES = 64 * 1024 * 1024
MAX_FRAMES = 640
U64 = 2**64 - 1


def require(condition, message):
    if not condition:
        raise ValueError(message)


def uint(value, maximum=U64):
    return type(value) is int and 0 <= value <= maximum


def keys(value, expected):
    require(type(value) is dict and set(value) == set(expected.split()), "invalid object fields")


def digest(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{16}", value) is not None,
            "invalid digest")


def key(value, minimum=-32768, maximum=32767):
    require(type(value) is list and len(value) == 2 and
            all(type(axis) is int and minimum <= axis <= maximum for axis in value), "invalid key")
    return tuple(value)


def rows(value, limit):
    require(type(value) is list and 0 < len(value) <= limit, "invalid row count")
    return value


def boundary_work(owner_key, owners, tick):
    # Both profiles use reduced shift 1. Shared edges run on even ticks,
    # independently of a Reduced owner's interior phase. Inspect four neighbours
    # in the already validated, at-most-256-owner roster; frozen cells never participate.
    if owners[owner_key]["state"] != 1 or tick % 2:
        return False
    x, z = owner_key
    return any(neighbour in owners and owners[neighbour]["state"] != 2 and
               owners[neighbour]["cells"] > 0
               for neighbour in ((x - 1, z), (x + 1, z), (x, z - 1), (x, z + 1)))


def validate(packet, *, full_route=False, measurement=False):
    keys(packet, "schema profile scheduler seed distance_m stride_m frames reopen")
    require(packet["schema"] == "luminumbra.field_page_evidence.v1", "invalid schema")
    require(packet["seed"] == "1337", "invalid seed")
    pressure = packet["profile"] == "diagnostic-pressure"
    require(pressure or packet["profile"] == "shipped-settings", "invalid profile")
    require(measurement or pressure, "freeze proof requires diagnostic pressure")
    keys(packet["scheduler"], "work_limit hold_ticks reduced_shift")
    config = packet["scheduler"]
    require(all(uint(config[name]) for name in config), "invalid scheduler")
    require(config == {"work_limit": 10 if pressure else U64,
                       "hold_ticks": 2 if pressure else 30, "reduced_shift": 1}, "invalid scheduler profile")
    distance, stride = packet["distance_m"], packet["stride_m"]
    require(uint(distance, 20000) and distance > 0 and uint(stride, 512) and stride >= 128,
            "invalid route")
    require(not full_route or distance == 20000, "full route requires 20000 m")
    frames = rows(packet["frames"], MAX_FRAMES)
    require(len(frames) == 4 * (1 + (distance + stride - 1) // stride), "truncated route")
    previous = {}
    reduced_since = {}
    pressure_run = 0
    activations = freezes = stable = completed = 0
    for index, frame in enumerate(frames):
        keys(frame, "tick anchor schedule_digest schedule_work activation_visits due owners masks fields_digest ledger_digest tick_us")
        tick = index + 1
        require(type(frame["tick"]) is int and frame["tick"] == tick, "invalid tick sequence")
        require(frame["anchor"] == [256 + min(distance, (index // 4) * stride), 0, 256] and
                all(type(axis) is int for axis in frame["anchor"]), "invalid walking anchor")
        for name in ("schedule_digest", "fields_digest", "ledger_digest"):
            digest(frame[name])
        require(uint(frame["tick_us"]) and uint(frame["schedule_work"]) and
                uint(frame["activation_visits"], 256), "invalid work or timing")
        pressure_run = pressure_run + 1 if frame["schedule_work"] > config["work_limit"] else 0
        require(type(frame["due"]) is list and len(frame["due"]) <= 256, "invalid due owners")
        due = [key(item) for item in frame["due"]]
        require(len(due) == len(set(due)) and frame["activation_visits"] == len(due), "invalid due visits")
        owners = {}
        for owner in rows(frame["owners"], 256):
            keys(owner, "key state cells digest activated frozen_at frozen_digest last_ticked completed_work")
            owner_key = key(owner["key"])
            require(owner_key not in owners, "duplicate owner")
            require(uint(owner["state"], 2) and uint(owner["cells"], 484) and
                    type(owner["activated"]) is bool and uint(owner["frozen_at"], tick) and
                    uint(owner["last_ticked"], tick) and uint(owner["frozen_digest"]) and
                    uint(owner["completed_work"]), "invalid owner")
            digest(owner["digest"])
            prior = previous.get(owner_key)
            if owner["state"] == 1 and (prior is None or prior["state"] != 1):
                reduced_since[owner_key] = tick
            require(prior is None or owner["cells"] >= prior["cells"], "lost initialized owner cells")
            activated = owner["cells"] > 0 and (prior is None or prior["cells"] == 0)
            require(owner["activated"] == activated, "invalid activation witness")
            if activated:
                require(owner_key in due, "activation outside due owners")
                activations += 1
            if owner["state"] == 2:
                require(owner["cells"] > 0 and prior is not None and
                        owner["digest"] == prior["digest"] and owner["cells"] == prior["cells"] and
                        0 < owner["frozen_at"] <= tick and owner["frozen_digest"] > 0 and
                        owner["last_ticked"] < owner["frozen_at"] and owner["completed_work"] == 0 and
                        owner_key not in due, "invalid frozen witness")
                require(prior["state"] in (1, 2), "freeze without reduced cadence")
                if prior["state"] == 2:
                    require(all(owner[name] == prior[name] for name in
                                ("frozen_at", "frozen_digest", "last_ticked")), "frozen stamps changed")
                elif pressure:
                    # ActiveRegionLedger selects using the preceding hold_ticks-1
                    # over ticks plus this tick, and resets holds on each transition.
                    # Reduced -> Frozen therefore needs a fresh hold after reduction.
                    require(pressure_run >= config["hold_ticks"], "freeze without held pressure")
                    require(tick - reduced_since[owner_key] >= config["hold_ticks"],
                            "freeze before reduced hold")
                freezes += prior["state"] != 2
                stable += prior["state"] == 2
            if owner["completed_work"]:
                require(owner["cells"] > 0, "work outside initialized authority")
                completed += owner["completed_work"]
            owners[owner_key] = owner
        require(list(owners) == sorted(owners) and previous.keys() <= owners.keys() and
                set(due) <= owners.keys(), "invalid owner roster")
        for owner_key, owner in owners.items():
            if owner["completed_work"] and owner_key not in due:
                require(boundary_work(owner_key, owners, tick), "work outside scheduled authority")
        mask_keys = []
        cells = dict.fromkeys(owners, 0)
        require(type(frame["masks"]) is list and len(frame["masks"]) <= 1024, "invalid mask count")
        for mask in frame["masks"]:
            keys(mask, "key words")
            page_x, page_z = key(mask["key"], -(2**31), 2**31 - 1)
            mask_keys.append((page_x, page_z))
            words = mask["words"]
            require(type(words) is list and len(words) == 4 and all(uint(word) for word in words),
                    "invalid mask")
            require(any(words), "empty mask")
            # FieldPageState::CellSlotOf / RegionAxisOfCell: 16x16 pages,
            # row-major bits, 24 m cells owned by their centres in 512 m regions.
            # At most 1024 * 256 bit visits per frame; no world scan.
            for word_index, word in enumerate(words):
                while word:
                    bit = (word & -word).bit_length() - 1
                    slot = word_index * 64 + bit
                    cell_x, cell_z = page_x * 16 + slot % 16, page_z * 16 + slot // 16
                    require(-699051 <= cell_x <= 699050 and -699051 <= cell_z <= 699050,
                            "mask cell outside field bounds")
                    owner_key = ((cell_x * 24000 + 12000) // 512000,
                                 (cell_z * 24000 + 12000) // 512000)
                    require(owner_key in owners, "mask owner outside roster")
                    cells[owner_key] += 1
                    word &= word - 1
        require(mask_keys == sorted(set(mask_keys)) and
                all(cells[owner_key] == owner["cells"] for owner_key, owner in owners.items()),
                "mask/owner cell mismatch")
        previous = owners
    require(activations > 0 and completed > 0, "zero activation or completed work")
    require(not pressure or (freezes > 0 and stable > 0), "zero freeze or repeated frozen observation")
    reopened = packet["reopen"]
    keys(reopened, "tick fields_digest ledger_digest exact_equal continuation_ticks save_us load_us")
    require(type(reopened["tick"]) is int and reopened["tick"] == len(frames) and
            reopened["exact_equal"] is True and type(reopened["continuation_ticks"]) is int and
            reopened["continuation_ticks"] == 2, "missing reopen equality")
    for name in ("fields_digest", "ledger_digest"):
        digest(reopened[name])
    require(all(reopened[name] == frames[-1][name] for name in ("fields_digest", "ledger_digest")),
            "reopen authority mismatch")
    require(uint(reopened["save_us"]) and uint(reopened["load_us"]), "invalid reopen timing")
    return {"frames": len(frames), "activations": activations, "freezes": freezes,
            "stable_frozen_observations": stable, "completed_work": completed,
            "profile": packet["profile"], "distance_m": distance}


def unique_object(pairs):
    result = {}
    for name, value in pairs:
        require(name not in result, "duplicate JSON field")
        result[name] = value
    return result


def parse(raw):
    require(len(raw) <= MAX_BYTES, "packet exceeds byte bound")
    try:
        return json.loads(raw, object_pairs_hook=unique_object,
                          parse_constant=lambda _: require(False, "non-finite JSON number"))
    except (UnicodeError, RecursionError, json.JSONDecodeError) as error:
        raise ValueError("malformed or truncated JSON packet") from error


def read_packet(path):
    # Open first, then inspect and read the same descriptor. O_NONBLOCK prevents
    # FIFO inputs hanging POSIX; fstat refuses non-regular inputs on both hosts.
    descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0))
    with os.fdopen(descriptor, "rb") as stream:
        status = os.fstat(stream.fileno())
        require(stat.S_ISREG(status.st_mode) and status.st_size <= MAX_BYTES, "invalid packet file")
        return parse(stream.read(MAX_BYTES + 1))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packet", type=Path)
    parser.add_argument("--measurement", action="store_true", help="allow shipped-settings observations")
    args = parser.parse_args()
    try:
        print(json.dumps(validate(read_packet(args.packet), full_route=True, measurement=args.measurement),
                         sort_keys=True))
        return 0
    except (OSError, ValueError) as error:
        print(f"field evidence refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
