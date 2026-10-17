"""Source checks for admission work that must remain outside steady ticks."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp"


def function(source, name):
    # Mask comments and literals before balancing production C++ braces.
    masked = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                    lambda match: " " * len(match.group()), source, flags=re.S)
    start = masked.index("{", source.index(name))
    depth = 1
    for index in range(start + 1, len(masked)):
        depth += (masked[index] == "{") - (masked[index] == "}")
        if depth == 0:
            return re.sub(r"\s+", " ", source[start + 1:index]).strip()
    raise AssertionError(f"Unclosed function {name}")


class RegionAuthorityWiring(unittest.TestCase):
    def test_persistence_pins_and_admits_owner_before_any_dereference(self):
        source = SOURCE.read_text(encoding="utf-8")
        persist = function(source, "bool SHIELD_WorldSystem::persist_authority(")
        self.assertIn("const auto chunk = incoming;", persist)
        self.assertIn("if (!chunk || !chunk->has_simulation_authority() || m_authorityFault) "
                      "return false;", persist)
        self.assertLess(persist.index("if (!chunk"), persist.index("chunk->get_id()"))
        self.assertIn("const auto owner = durable->second.owner.lock();", persist)
        self.assertIn("owner && owner == chunk && "
                      "durable->second.stamp == owner->authority_stamp()", persist)
        self.assertNotIn("owner.lock() == chunk", source)
        cache_source = (ROOT / "src/luminumbra_common/persistence/"
                        "WorldPersistenceRoundtrip.cpp").read_text(encoding="utf-8")
        cache = function(cache_source, "void AuthorityHashCache::update(")
        self.assertIn("const auto chunk = incoming;", cache)
        self.assertIn("if (!chunk || !chunk->has_simulation_authority()) return;", cache)
        self.assertIn("const bool same_owner = owner && owner == chunk;", cache)
        self.assertEqual(cache.count("owner.lock()"), 1)

    def test_refused_payloads_stage_before_creating_destination_directories(self):
        source = (ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp").read_text(
            encoding="utf-8")
        for name, interruption in (
                ("bool WorldSaveService::write_file_durably(",
                 "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool RewriteRegionFile(", "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool WriteWorldManifest(", "InterruptBeforeRegionReplaceForTesting().exchange"),
                ("bool WorldSaveService::save_regions_validated(",
                 "BeforeActiveRegionsReplaceForTesting()")):
            with self.subTest(writer=name):
                writer = function(source, name)
                self.assertLess(writer.index("WriteDurableRegionTemp"),
                                writer.index(interruption))
                self.assertIn("CreatePayloadDirectory", writer)
                self.assertLess(writer.index(interruption), writer.index("CreatePayloadDirectory"))
                self.assertLess(writer.index("CreatePayloadDirectory"),
                                writer.index("ReplaceRegionFileAtomically"))
                self.assertNotIn("create_directories", writer)
        producer = function(source, "bool WriteDurableRegionTempWithProducer(")
        self.assertLess(producer.index("RegionStagingDestination"),
                        producer.index("OpenDurableRegionTemp(staging"))
        snapshot = function(source, "bool WorldSaveService::write_snapshot(")
        self.assertNotIn("create_directories", snapshot)

    def test_eviction_scan_is_reachable_only_after_capacity_check(self):
        source = SOURCE.read_text(encoding="utf-8")
        room = function(source, "bool SHIELD_WorldSystem::make_authority_room(")
        self.assertTrue(room.startswith(
            "if (resident_chunk_count() < STREAMING_MAX_ACTIVE_CHUNKS_BUDGET) return true; "
            "if (!eviction_prepared) { prepare_authority_eviction(required); "
            "eviction_prepared = true; }"))
        # Includes the definition: there must be no eager caller in fixed ticks,
        # readiness, edits, or migration. This checks shipping source, not a model.
        self.assertEqual(source.count("prepare_authority_eviction("), 2)

    def test_pressure_eviction_keeps_the_stable_order_and_shared_cap(self):
        source = SOURCE.read_text(encoding="utf-8")
        prepare = function(source, "void SHIELD_WorldSystem::prepare_authority_eviction(")
        self.assertIn("std::sort(m_authorityEvictionCandidates.rbegin(), "
                      "m_authorityEvictionCandidates.rend());", prepare)
        room = function(source, "bool SHIELD_WorldSystem::make_authority_room(")
        self.assertIn("m_authorityEvictionCandidates.back()", room)
        self.assertIn("m_authorityEvictionCandidates.pop_back();", room)
        header = SOURCE.with_suffix(".h").read_text(encoding="utf-8")
        self.assertRegex(header, r"kActiveChunkBudget\s*=\s*8192")
        native = (ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(encoding="utf-8")
        self.assertIn("EvictionPreparationRequiresAdmissionPressure", native)
        self.assertIn("authority_eviction_preparations()", native)


if __name__ == "__main__":
    unittest.main()
