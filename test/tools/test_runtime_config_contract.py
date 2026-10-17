"""Keep entry-point runtime loading wired to the exercised production loader."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def compact_cpp(source):
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                        re.DOTALL)
    source = tokens.sub(lambda match: "" if match[0].startswith(("//", "/*"))
                        else match[0], source)
    return re.sub(r"\s+", "", source)


class RuntimeConfigSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.client = compact_cpp((ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8"))
        cls.runner = compact_cpp((ROOT / "src/luminumbra_server/ServerWorldRunner.cpp").read_text(encoding="utf-8"))
        cls.admission = compact_cpp((ROOT / "src/luminumbra_server/modes/ModeHelpers.cpp").read_text(encoding="utf-8"))

    def test_client_loads_runtime_defaults_before_explicit_override(self):
        load = ("g_systemConfig=luminumbra::core::SystemConfig::LoadFromRuntimeRoot("
                "root_dir,luminumbra::core::SystemConfig::DefaultUserOverlayPath());")
        self.assertIn(load, self.client)
        self.assertLess(self.client.index(load),
                        self.client.index("if(scenario_config.systems_config_override){"))
        override = "if(scenario_config.systems_config_override){"
        self.assertIn(override +
                      "g_systemConfig.ReplaceSystemsFrom(*scenario_config.systems_config_override);}",
                      self.client)
        self.assertNotIn("g_systemConfig=*scenario_config.systems_config_override;", self.client)

    def test_server_boot_and_admission_share_root_loader(self):
        for source, root in ((self.runner, "m_config.root_path"), (self.admission, "root")):
            self.assertIn("luminumbra::core::SystemConfig::LoadFromRuntimeRoot(" + root + ")",
                          source)
            self.assertNotIn('"data/common/systems.json"', source)

    def test_scenario_host_uses_runtime_defaults_unless_explicitly_overridden(self):
        host = compact_cpp((ROOT / "src/luminumbra_client/core/scenarios/NetworkedSessionDriver.cpp")
                           .read_text(encoding="utf-8"))
        self.assertIn("m_impl->host_session->ApplySystemConfig("
                      "config.systems_config_override?*config.systems_config_override:"
                      "luminumbra::core::SystemConfig::LoadFromRuntimeRoot(config.root_path));", host)
        self.assertNotIn('"data/common/systems.json"', host)
        self.assertNotIn("SystemConfig::LoadFromFile(", host)

    def test_ui_watchers_use_runtime_root(self):
        for extension in ("rml", "rcss"):
            self.assertIn('uiHotReload.WatchDirectory(root_path_str+"data/ui","' +
                          extension + '");', self.client)
            self.assertNotIn('uiHotReload.WatchDirectory("data/ui","' + extension + '");',
                             self.client)

    def test_preset_rename_refuses_removal_errors_before_reporting_success(self):
        writer = compact_cpp((ROOT / "src/luminumbra_client/world/WorldgenOverride.cpp")
                             .read_text(encoding="utf-8"))
        self.assertIn("Luminumbra::Client::RenameUserPreset(src,dst,newDisplayName)", self.client)
        rename = writer[writer.index("boolRenameUserPreset("):]
        publish = rename.index("if(!Persistence::WorldSaveService::write_file_durably(")
        removal = rename.index("std::filesystem::remove(source,error);")
        self.assertLess(publish, removal)
        identity = rename.index("std::filesystem::equivalent(source,destination,error)")
        self.assertLess(publish, identity)
        self.assertLess(identity, removal)
        self.assertIn("returnfalse;", rename[publish:removal])
        self.assertIn("returnremoved&&!error;", rename[removal:])

    def test_preset_and_settings_writers_use_checked_atomic_publication(self):
        start = self.client.index("g_uiManager->SetWorldPresetSaver(")
        end = self.client.index("g_uiManager->SetWorldPresetList(", start)
        self.assertIn("WorldSaveService::write_file_durably(", self.client[start:end])
        self.assertNotIn("std::ofstream", self.client[start:end])
        self.assertLess(self.client[start:end].index("in.close();"),
                        self.client[start:end].index("WorldSaveService::write_file_durably("))
        settings = compact_cpp((ROOT / "src/luminumbra_common/core/SystemConfig.cpp")
                               .read_text(encoding="utf-8"))
        save = settings[settings.index("boolSystemConfig::SaveUserOverlay("):]
        self.assertIn("WorldSaveService::write_file_durably(path,serialized)", save)

    def test_plant_staging_is_reclaimed_outside_the_strict_region_scan(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        start = service.index("boolWorldSaveService::save_plant_entities(")
        end = service.index("boolWorldSaveService::load_plant_entities(", start)
        save = service[start:end]
        self.assertIn("constautostaging=save_dir/path.filename();", save)
        self.assertLess(save.index("ReclaimAbandonedStaging(staging);"),
                        save.index("if(snapshot.entities.empty())"))
        self.assertIn("WriteFileDurably(path,", save)
        self.assertIn(",staging,errors)", save)

    def test_region_and_manifest_staging_share_the_unscanned_save_root(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        self.assertIn("constautonormalized=Filesystem::LexicallyNormalPath(destination);", service)
        self.assertNotIn(".lexically_normal()", service)
        start = service.index("std::size_tReclaimAbandonedStaging(")
        end = service.index("returnreclaimed;", start)
        self.assertIn('destination.has_parent_path()?destination.parent_path():'
                      'std::filesystem::path(".")', service[start:end])
        self.assertIn("returndirectory.parent_path().parent_path()/normalized.filename();", service)
        for function, destination in (("WriteRegionFile", "path"),
                                      ("WriteWorldManifest", "manifest_path")):
            start = service.index("bool" + function + "(")
            end = service.index("returntrue;", start)
            write = service[start:end]
            self.assertIn("constautostaging=WorldSaveService::region_staging_path(" +
                          destination + ");", write)
            self.assertIn("ReclaimAbandonedStaging(staging);", write)
            self.assertIn("WriteDurableRegionTemp(staging,", write)

    def test_writers_hold_shared_ownership_through_cleanup_and_publication(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        for name in ("save_metadata", "save_metadata_and_active_regions", "save_plant_entities",
                     "save_active_regions", "upsert_container_records", "save_dirty_chunks",
                     "write_snapshot", "write_file_durably", "remove_file_and_staging"):
            start = service.index("WorldSaveService::" + name + "(")
            body = service.index("{", start)
            self.assertTrue(service[body:].startswith("{constSaveGuardguard;"), name)
        self.assertIn("std::recursive_mutex&SaveMutex()", service)
        self.assertIn("m_lock(SaveMutex(),std::defer_lock)", service)
        self.assertIn("m_lock.lock();", service)
        self.assertNotIn("PairedSaveGuard", service)
        # Scope includes the real read/merge, not just temporary-file creation.
        upsert = service[service.index("boolWorldSaveService::upsert_container_records("):]
        self.assertLess(upsert.index("constSaveGuardguard;"),
                        upsert.index("ReadRegionFile("))
        shared = service[service.index("boolWorldSaveService::write_file_durably("):]
        self.assertLess(shared.index("constSaveGuardguard;"),
                        shared.index("ReclaimAbandonedStaging(destination);"))
        self.assertLess(shared.index("ReclaimAbandonedStaging(destination);"),
                        shared.index("WriteFileDurably("))

    def test_energy_empty_and_nonempty_saves_share_owned_cleanup(self):
        session = compact_cpp((ROOT / "src/luminumbra_common/world/GameSession.cpp")
                              .read_text(encoding="utf-8"))
        start = session.index("boolGameSession::SaveEnergyFieldRecord(")
        end = session.index("std::stringGameSession::ComputeAetherStateSubHash", start)
        energy = session[start:end]
        self.assertIn('WorldSaveService::remove_file_and_staging(save_dir/"aether_state.efs")',
                      energy)
        self.assertIn('WorldSaveService::write_file_durably(save_dir/"aether_state.efs",record)',
                      energy)
        self.assertNotIn("fs::remove(", energy)
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        ledger = service[service.index("boolWorldSaveService::save_active_regions("):]
        self.assertLess(ledger.index("ReclaimAbandonedStaging(staging);"),
                        ledger.index("if(ledger.records().empty()"))
        remove = service[service.index("boolWorldSaveService::remove_file_and_staging("):]
        self.assertLess(remove.index("ReclaimAbandonedStaging(destination);"),
                        remove.index("std::filesystem::remove(destination,error);"))

    def test_roundtrip_export_requests_a_complete_snapshot(self):
        scenario = compact_cpp((ROOT / "src/luminumbra_client/core/scenarios/PersistenceRoundtrip.cpp")
                               .read_text(encoding="utf-8"))
        self.assertIn("SaveWorldStateTo(config.persistence_session_dir,&save_report,true)", scenario)

    def test_client_complete_saves_include_metadata(self):
        exit_save = compact_cpp((ROOT / "src/luminumbra_client/app/SessionExit.h")
                                .read_text(encoding="utf-8"))
        self.assertIn("if(session->SaveWorld())mark_saved();", exit_save)
        self.assertIn("if(!gameSession->SaveWorld()){", self.client)

    def test_new_fixture_publishes_metadata_after_required_state(self):
        fixture = compact_cpp((ROOT / "src/luminumbra_common/persistence/SyntheticSaveFixture.cpp")
                              .read_text(encoding="utf-8"))
        self.assertLess(fixture.index("WorldSaveService{}.save_world("),
                        fixture.index("WorldSaveService::save_metadata("))

    def test_empty_plant_roster_checks_removal_error(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        start = service.index("if(snapshot.entities.empty()){")
        end = service.index("returntrue;", start)
        empty_roster = service[start:end]
        self.assertIn("std::filesystem::remove(path,ec);if(ec){", empty_roster)
        self.assertIn("returnfalse;", empty_roster)


if __name__ == "__main__":
    unittest.main()
