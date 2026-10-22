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

    def test_replay_admission_freezes_runtime_defaults_before_identity_matching(self):
        replay = self.admission[self.admission.index("ReplayRunnerConfigFrom("):]
        load = ("options.systems_config_override?*options.systems_config_override:"
                "luminumbra::core::SystemConfig::LoadFromRuntimeRoot(root);")
        self.assertIn(load, replay)
        self.assertLess(replay.index(load),
                        replay.index("config.systems_config_override=systems_config;"))
        self.assertLess(replay.index("config.systems_config_override=systems_config;"),
                        replay.index("identity_session.ApplySystemConfig(systems_config);"))
        self.assertNotIn("SystemConfig::LoadFromFile(", replay)

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
            self.assertIn('uiHotReload.WatchDirectory(root_dir/"data/ui","' +
                          extension + '");', self.client)
            self.assertNotIn('uiHotReload.WatchDirectory("data/ui","' + extension + '");',
                             self.client)

    def test_preset_rename_refuses_occupied_targets_before_publication(self):
        writer = compact_cpp((ROOT / "src/luminumbra_client/world/WorldgenOverride.cpp")
                             .read_text(encoding="utf-8"))
        self.assertIn("Luminumbra::Client::RenameUserPreset(src,dst,newDisplayName)", self.client)
        self.assertIn("WorldSaveService::rename_file_durably("
                      "source,destination,original,preset.dump(2))", writer)
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        rename = service[service.index("boolWorldSaveService::rename_file_durably("):]
        rename = rename[:rename.index("boolWorldSaveService::remove_file_and_staging(")]
        self.assertIn("{constSaveGuardguard;", rename)
        self.assertLess(rename.index("contents.str()!=expected_bytes"),
                        rename.index("ReclaimAbandonedStaging(destination)"))
        self.assertLess(rename.index("std::filesystem::equivalent(source,destination,error)"),
                        rename.index("WriteFileDurably(destination,bytes,destination,errors,&published_identity)"))
        self.assertIn('AddError(errors,"renamedestinationisoccupiedorcannotbeinspected");'
                      'returnfalse;', rename)
        self.assertLess(rename.index("publication.Publish(errors)"),
                        rename.rindex("std::filesystem::remove(source,error)"))
        self.assertLess(rename.rindex("std::filesystem::remove(source,error)"),
                        rename.index("publication.Commit()"))
        self.assertIn("returnFlushRemovalDirectory(source,errors);", rename)
        self.assertIn("std::filesystem::create_hard_link(destination,candidate,error)", rename)
        self.assertIn("MatchesFileIdentity(destination,published_identity)", rename)
        self.assertIn("ReplaceRegionFileAtomically(backup,destination,errors)", rename)

    def test_no_replace_publication_is_atomic_on_both_platforms(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        publish = service[service.index("classNewFilePublication{"):]
        publish = publish[:publish.index("boolCompressPayload(")]
        self.assertIn("::link(m_temp.c_str(),m_destination.c_str())", publish)
        self.assertIn("::syscall(SYS_renameat2,AT_FDCWD,m_temp.c_str(),AT_FDCWD,"
                      "m_destination.c_str(),RENAME_NOREPLACE)", publish)
        self.assertIn("::renamex_np(m_temp.c_str(),m_destination.c_str(),RENAME_EXCL)", publish)
        self.assertIn("errno==ENOSYS||errno==EINVAL||errno==EOPNOTSUPP", publish)
        self.assertIn("::MoveFileExW(m_temp.c_str(),m_destination.c_str(),"
                      "MOVEFILE_WRITE_THROUGH)", publish)
        self.assertNotIn("MOVEFILE_REPLACE_EXISTING", publish)
        self.assertNotIn("std::rename(", publish)
        self.assertIn("::fstat(m_descriptor,&published)==0", publish)
        self.assertIn("::lstat(m_destination.c_str(),&named)==0", publish)
        self.assertIn("published.st_dev==named.st_dev&&published.st_ino==named.st_ino", publish)
        self.assertIn("m_published&&!m_committed", publish)
        self.assertIn("::SetFileInformationByHandle(m_handle,FileDispositionInfo,", publish)
        self.assertIn("returnFlushRemovalDirectory(m_destination,errors);", publish)

    def test_every_replacement_flushes_ancestor_entries_before_publication(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        parts = service.split("boolReplaceRegionFileAtomically(")[1:]
        self.assertEqual(len(parts), 2)
        for implementation, syscall in zip(parts, ("::GetFileAttributesW(", "std::rename(")):
            self.assertLess(implementation.index("if(!FlushAncestorEntries(destination,errors))"),
                            implementation.index(syscall))
        remove = service[service.index("boolWorldSaveService::remove_file_and_staging("):]
        self.assertIn("returnFlushRemovalDirectory(destination,errors);", remove)
        flush = service[service.index("boolFlushRemovalDirectory(",
                                      service.index("boolFlushRemovalDirectory(") + 1):]
        self.assertIn("return!exists||FlushDirectoryEntry(parent,errors);", flush)

    def test_config_io_retains_native_paths(self):
        header = compact_cpp((ROOT / "src/luminumbra_common/core/SystemConfig.h")
                             .read_text(encoding="utf-8"))
        source = compact_cpp((ROOT / "src/luminumbra_common/core/SystemConfig.cpp")
                             .read_text(encoding="utf-8"))
        for name, argument in (("LoadFromFile", "path"), ("TryLoadFromFile", "path"),
                               ("SaveUserOverlay", "path"), ("LoadLayered", "defaults_path")):
            self.assertIn(name + "(conststd::filesystem::path&" + argument, header)
            self.assertIn("SystemConfig::" + name + "(conststd::filesystem::path&" + argument,
                          source)
        self.assertIn('returnLoadLayered(root/"data/common/systems.json",overlay_path);', source)
        self.assertNotIn("LoadLayered(Luminumbra::Filesystem::GenericPathString(", source)
        self.assertIn("std::ifstreamfile(path);", source)
        self.assertIn("std::ifstreamoverlay(overlay_path,std::ios::binary);", source)
        self.assertIn("::GetEnvironmentVariableW(L\"APPDATA\"", source)
        service = (ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp").read_text(
            encoding="utf-8")
        self.assertNotIn(".string()", service)
        watcher = compact_cpp((ROOT / "src/luminumbra_client/ui/core/UIHotReload.cpp")
                              .read_text(encoding="utf-8"))
        self.assertIn("WatchDirectory(conststd::filesystem::path&directoryPath", watcher)
        self.assertIn("constautopath=entry.path();", watcher)
        self.assertNotIn("entry.path().string()", watcher)

    def test_saver_consent_is_bound_to_confirmed_name(self):
        ui = compact_cpp((ROOT / "src/luminumbra_client/ui/Rml_UIManager.cpp")
                         .read_text(encoding="utf-8"))
        start = ui.index('if(auto*confirm=document->GetElementById("confirm_overwrite_btn"))')
        confirm = ui[start:ui.index('if(auto*cancel=', start)]
        self.assertIn("if(!m_pendingPresetOverwrite)return;", confirm)
        self.assertIn("conststd::stringname=*m_pendingPresetOverwrite;", confirm)
        self.assertIn("CommitSavePreset(document,name,true)", confirm)
        self.assertNotIn("ReadFormControlValue", confirm)
        self.assertIn("m_worldPresetSaver(name,baseType,CollectWorldGenParams(document),"
                      "overwrite_confirmed)", ui)
        self.assertIn("overwrite_confirmed?Writer::write_file_durably(destination,bytes):"
                      "Writer::create_file_durably(destination,bytes)", self.client)

    def test_preset_and_settings_writers_use_checked_atomic_publication(self):
        start = self.client.index("g_uiManager->SetWorldPresetSaver(")
        end = self.client.index("g_uiManager->SetWorldPresetList(", start)
        self.assertIn("Writer::write_file_durably(", self.client[start:end])
        self.assertIn("Writer::create_file_durably(", self.client[start:end])
        self.assertNotIn("std::ofstream", self.client[start:end])
        self.assertLess(self.client[start:end].index("in.close();"),
                        self.client[start:end].index("Writer::write_file_durably("))
        settings = compact_cpp((ROOT / "src/luminumbra_common/core/SystemConfig.cpp")
                               .read_text(encoding="utf-8"))
        save = settings[settings.index("boolSystemConfig::SaveUserOverlay("):]
        self.assertIn("WorldSaveService::write_file_durably(path,serialized)", save)

    def test_overlay_failure_regression_reaches_the_owned_staging_writer(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        start = service.index("boolWriteFileDurably(", service.index("boolWriteFileDurably(") + 1)
        write = service[start:service.index("boolWorldSaveService::write_file_durably(", start)]
        interrupt = "InterruptBeforeRegionReplaceForTesting().exchange(false,std::memory_order_acq_rel)"
        self.assertLess(write.index("WriteDurableRegionTemp(staging,bytes,temp_path,errors)"),
                        write.index(interrupt))
        self.assertLess(write.index(interrupt), write.index("ReplaceRegionFileAtomically("))
        interrupted = write[write.index(interrupt):write.index("ReplaceRegionFileAtomically(")]
        self.assertIn("RemoveTemporaryFile(temp_path);", interrupted)
        self.assertIn("returnfalse;", interrupted)
        fixture = compact_cpp((ROOT / "test/common/GraphicsPresets_test.cpp")
                              .read_text(encoding="utf-8"))
        start = fixture.index("TEST(GraphicsPresets,SaveFailuresPreserveExistingFileAndCleanTemporaryOutput)")
        failure = fixture[start:fixture.index("TEST(GraphicsPresets,EveryPreset", start)]
        self.assertIn("constOverlayWriteResetreset;", failure)
        armed = failure.index("set_interrupt_before_region_replace_for_testing(true)")
        refused = failure.index("EXPECT_FALSE(config.SaveUserOverlay(", armed)
        disarmed = failure.index("set_interrupt_before_region_replace_for_testing(false)", refused)
        self.assertLess(armed, refused)
        self.assertLess(refused, disarmed)
        self.assertIn("EXPECT_EQ(ReadText(file.Path()),original);", failure[disarmed:])
        self.assertIn("ASSERT_TRUE(config.SaveUserOverlay(file.Path()));", failure[disarmed:])
        self.assertIn("ExpectNoOverlayStaging(file.Path());", failure[disarmed:])
        self.assertIn('prefix+=".tmp.";', fixture)
        self.assertIn("entry.path().filename().native().find(prefix.native())", fixture)

    def test_metadata_cleanup_fixture_exercises_validated_clock_bytes(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        start = service.index("boolWorldSaveService::save_metadata(")
        metadata = service[start:service.index("boolWorldSaveService::save_metadata_and_active_regions(", start)]
        reclaim = metadata.index("ReclaimAbandonedStaging(destination);")
        for validation in ("validate_save(save_dir,errors)",
                           "world::WorldClock::from_metadata(metadata,clock,clock_error)",
                           "ReadAmbientAnchor(metadata,ambient_anchor,clock_error)",
                           "read_clock_metadata(save_dir,previous_clock,requires_active_regions,errors)",
                           "if(requires_active_regions&&!has_clock)"):
            self.assertLess(metadata.index(validation), reclaim)
        self.assertLess(reclaim, metadata.index("WriteDurableRegionTemp("))
        fixture = compact_cpp((ROOT / "test/persistence/world_save_service_test.cpp")
                              .read_text(encoding="utf-8"))
        start = fixture.index("TEST(WorldSaveService,EmptyLedgerAndMetadataSavesReclaimBeforeTheirWriteOrNoop)")
        cleanup = fixture[start:fixture.index("TEST(WorldSaveService,LoadFromEmpty", start)]
        self.assertIn("clock.write_metadata(metadata);", cleanup)
        first_save = "ASSERT_TRUE(WorldSaveService::save_metadata(metadata.dump(),save_dir.path()));"
        baseline = 'constautovalid=ReadFileBytes(save_dir.path()/"world_info.json");'
        self.assertLess(cleanup.index(first_save), cleanup.index(baseline))
        self.assertIn("ASSERT_EQ(nlohmann::json::parse(valid),metadata);", cleanup)
        self.assertLess(cleanup.index(baseline), cleanup.index("for(constauto*invalid:"))
        self.assertEqual(cleanup.count("ASSERT_TRUE(WorldSaveService::save_metadata(valid,save_dir.path()));"), 1)
        self.assertNotIn('ASSERT_TRUE(WorldSaveService::save_metadata("{}",', cleanup)
        self.assertIn('EXPECT_FALSE(WorldSaveService::save_metadata("{}",save_dir.path()));', cleanup)
        self.assertIn('EXPECT_EQ(ReadFileBytes(metadata_staging),"partialmetadata");', cleanup)

    def test_recursive_watcher_regression_uses_native_paths_and_observed_parent_time(self):
        watcher = compact_cpp((ROOT / "src/luminumbra_client/ui/core/UIHotReload.cpp")
                              .read_text(encoding="utf-8"))
        self.assertIn("extensions.insert(extension);", watcher)
        self.assertIn("std::filesystem::recursive_directory_iterator(directoryPath)", watcher)
        self.assertIn("if(m_watchedFiles.contains(path))continue;", watcher)
        self.assertIn("m_reloadCallback(Filesystem::GenericUtf8PathString(filePath));", watcher)
        fixture = compact_cpp((ROOT / "test/persistence/world_open_refusal_test.cpp")
                              .read_text(encoding="utf-8"))
        start = fixture.index("TEST_F(WorldOpenRefusal,DirectoryWatchesRetainBothFiltersAndDiscoverNestedFiles)")
        discovery = fixture[start:fixture.index("TEST_F(WorldOpenRefusal,DirectoryWatchWildcard", start)]
        self.assertIn("ASSERT_EQ(fs::last_write_time(ui),parent_time);", discovery)
        self.assertNotIn("fs::last_write_time(ui,", discovery)
        self.assertIn('reload.WatchDirectory(ui,"rml");', discovery)
        self.assertIn('reload.WatchDirectory(ui,".rcss");', discovery)
        self.assertIn("Filesystem::GenericUtf8PathString(documents[0])", discovery)
        self.assertIn("Filesystem::GenericUtf8PathString(documents[1])", discovery)
        wildcard = fixture[fixture.index("TEST_F(WorldOpenRefusal,DirectoryWatchWildcard", start):]
        self.assertIn('Filesystem::GenericUtf8PathString(ui/"new.txt")', wildcard)

    def test_cancelled_validation_fixture_owns_a_mutable_stop_source(self):
        fixture = compact_cpp((ROOT / "test/persistence/world_open_refusal_test.cpp")
                              .read_text(encoding="utf-8"))
        start = fixture.index("TEST_F(WorldOpenRefusal,CancelledValidationNeverReportsAValidSaveOrLeavesPartialAuthority)")
        cancellation = fixture[start:fixture.index("TEST_F(WorldOpenRefusal,InvalidMetadata", start)]
        self.assertIn("std::stop_sourcecancellation;", cancellation)
        self.assertNotIn("conststd::stop_sourcecancellation;", cancellation)
        self.assertIn("ASSERT_TRUE(cancellation.request_stop());", cancellation)
        self.assertIn("ASSERT_TRUE(cancellation.stop_requested());", cancellation)

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
        self.assertIn("constautodirectory=destination.parent_path();", service)
        self.assertNotIn(".lexically_normal()", service)
        start = service.index("std::size_tReclaimAbandonedStaging(")
        end = service.index("returnreclaimed;", start)
        self.assertIn('destination.has_parent_path()?destination.parent_path():'
                      'std::filesystem::path(".")', service[start:end])
        self.assertIn("returndirectory.parent_path().parent_path()/destination.filename();", service)
        self.assertNotIn('/".."/".."', service)
        for function, destination in (("WriteRegionFile", "path"),
                                      ("WriteWorldManifest", "manifest_path")):
            start = service.index("bool" + function + "(")
            end = service.index("returntrue;", start)
            write = service[start:end]
            self.assertIn("constautostaging=WorldSaveService::region_staging_path(" +
                          destination + ");", write)
            self.assertIn("ReclaimAbandonedStaging(staging);", write)
            self.assertIn("WriteDurableRegionTemp(staging,", write)

    def test_session_save_owns_validation_payloads_metadata_and_acknowledgement(self):
        session = compact_cpp((ROOT / "src/luminumbra_common/world/GameSession.cpp")
                              .read_text(encoding="utf-8"))
        start = session.index("boolGameSession::SaveWorldStateToImpl(")
        sequence = session[start:session.index("boolGameSession::LoadWorldState()", start)]
        guard = "constPersistence::WorldSaveService::SaveOwnershipsave_ownership;"
        self.assertIn(guard, sequence)
        ownership = sequence.index(guard)
        # Function scope keeps ownership across all phases and every early return.
        self.assertEqual(sequence[:ownership].count("{") - sequence[:ownership].count("}"), 1)
        for capture in ("quiesce_streaming_jobs_for_save();", "BuildPlantEntitySnapshot(m_registry)",
                        "state.dirty_chunk_ids()"):
            self.assertLess(sequence.index(capture), ownership)
        for operation in ("ClockConfigurationCompatible(save_dir)",
                          "validate_save(save_dir)",
                          "SavedPresetCompatible(save_dir)", "service.save_world(",
                          ".save_dirty_chunks(", "save_plant_entities(", "SaveEnergyFieldRecord(",
                          "SaveWorldMetadataTo(", "ReconcileWorldStateMetadataTo(",
                          "m_activeRegionLedger.mark_saved(",
                          "chunk->clear_voxel_data_dirty()"):
            self.assertLess(ownership, sequence.index(operation))

    def test_writers_hold_shared_ownership_through_cleanup_and_publication(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        for name in ("save_metadata", "save_metadata_and_active_regions", "save_plant_entities",
                     "save_active_regions", "upsert_container_records", "save_dirty_chunks",
                     "write_snapshot", "write_file_durably", "remove_file_and_staging",
                     "create_file_durably", "rename_file_durably"):
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
        end = service.index("returnFlushRemovalDirectory(path,errors);", start)
        empty_roster = service[start:end]
        self.assertIn("std::filesystem::remove(path,ec);if(ec){", empty_roster)
        self.assertIn("returnfalse;", empty_roster)
        self.assertIn("returnFlushRemovalDirectory(path,errors);", service[end:])


if __name__ == "__main__":
    unittest.main()
