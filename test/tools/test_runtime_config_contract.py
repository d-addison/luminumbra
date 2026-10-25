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
        for implementation, syscall in zip(parts, ("::MoveFileExW(", "std::rename(")):
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

    def test_far_snapshot_owner_paths_leave_durable_reads_to_the_worker(self):
        world = compact_cpp((ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
                            .read_text(encoding="utf-8"))
        start = world.index("SHIELD_WorldSystem::capture_far_lod_sdf_snapshot(")
        end = world.index("SHIELD_WorldSystem::find_streamed_chunk(", start)
        owner_paths = world[start:end]
        for disk_lookup in ("read_committed_chunk(", "find_authoritative_chunk(",
                            "m_durable_chunk_loader(", "m_durable_chunks"):
            self.assertNotIn(disk_lookup, owner_paths)
        self.assertIn("snapshot_authoritative_chunks()", owner_paths)
        self.assertIn("snapshot.authority_revision!=", owner_paths)
        worker = compact_cpp((ROOT / "src/luminumbra_client/rendering/FarLodSystem.cpp")
                             .read_text(encoding="utf-8"))
        start = worker.index("FarLodWorkerBuildOutcomeBuildFarLodWorkerTile(")
        durable_read = worker.index("read_authoritative_region_chunks(", start)
        worker = worker[start:worker.index("returnoutcome;", durable_read)]
        self.assertIn("neighbor_z=rz-1;neighbor_z<=rz+1;", worker)
        self.assertIn("neighbor_x=rx-1;neighbor_x<=rx+1;", worker)
        self.assertIn("read_authoritative_region_chunks(", worker)

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
                          "transaction->defer_until_commit(acknowledge)",
                          "acknowledge()", "outer->Commit()"):
            self.assertLess(ownership, sequence.index(operation))
        wrapper_start = session.index("boolGameSession::RunInSnapshotTransaction(")
        wrapper = session[wrapper_start:session.index("boolGameSession::SaveWorld()", wrapper_start)]
        self.assertIn("!m_worldUsable", wrapper)
        self.assertNotIn("m_worldOpenError", wrapper)
        self.assertLess(wrapper.index("quiesce_streaming_jobs_for_save();"), wrapper.index(guard))
        self.assertLess(wrapper.index(guard), wrapper.index("StartOuterSave("))
        metadata_start = session.index("boolGameSession::SaveWorldMetadataTo(")
        metadata = session[metadata_start:session.index("GameSession::GetSaveRootPath", metadata_start)]
        self.assertLess(metadata.index("saved_ledger.mark_saved(m_worldClock)"),
                        metadata.index("save_metadata_and_active_regions("))
        self.assertLess(metadata.index("transaction->defer_until_commit("),
                        metadata.index("outer->Commit()"))
        self.assertIn("m_activeRegionLedger.canonical_bytes()==expected_ledger", metadata)
        self.assertLess(metadata.index("if(SameSaveDestination(save_dir,GetWorldSaveDir()))"),
                        metadata.index("transaction->defer_until_commit("))
        self.assertLess(sequence.index("ReconcileWorldStateMetadataTo("),
                        sequence.index("transaction->defer_until_commit(acknowledge)"))

    def test_writers_hold_shared_ownership_through_cleanup_and_publication(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        for name in ("save_metadata", "save_metadata_and_active_regions", "save_plant_entities",
                     "save_active_regions", "upsert_container_records", "save_dirty_chunks",
                     "write_snapshot", "write_file_durably", "remove_file_and_staging",
                     "create_file_durably", "rename_file_durably", "save_ground_objects",
                     "save_field_pages", "durable_write_file", "durable_remove",
                     "durable_flush_directory", "write_member_bytes", "remove_member"):
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

    def test_region_scheduler_records_both_modes_without_requiring_a_world(self):
        session = compact_cpp((ROOT / "src/luminumbra_common/world/GameSession.cpp")
                              .read_text(encoding="utf-8"))
        tick = session[session.index("std::uint32_tGameSession::TickSimulation("):]
        schedule = tick.index("RunRegionScheduler(current_tick);")
        self.assertTrue("try{RunRegionScheduler(current_tick);" in tick,
                        "Each tick must enter the scheduler in both feature modes")
        self.assertLess(schedule, tick.index("if(m_activeRegionsEnabled&&m_worldSystem)"))
        scheduler = session[session.index("voidGameSession::RunRegionScheduler("):]
        scheduler = scheduler[:scheduler.index("std::vector<RegionKey>GameSession::SortedDueRegions")]
        self.assertIn("if(m_worldSystem){m_worldSystem->note_persistence_tick(", scheduler)
        self.assertIn("if(!m_worldSystem)returnPersistence::WorldSaveService::region_simulation_digest(",
                      scheduler)

    def test_legacy_energy_adoption_precedes_the_chunkless_return(self):
        session = compact_cpp((ROOT / "src/luminumbra_common/world/GameSession.cpp")
                              .read_text(encoding="utf-8"))
        adopt = session[session.index("voidGameSession::AdoptWorldStateFrom("):]
        fresh = adopt.index("if(!saved.snapshot_present&&!m_activeRegionsEnabled)")
        reset_call = "m_energyFieldState->ResetEmptyAtTick(GetSimulationTickCount());"
        self.assertTrue(reset_call in adopt, "Legacy adoption must clear reused energy state")
        reset = adopt.index(reset_call)
        restore = adopt.index("m_energyFieldState->DeserializeRecord(")
        self.assertLess(reset, restore)
        self.assertLess(restore, fresh)
        normal = session[session.index("boolGameSession::LoadWorld("):]
        normal = normal[:normal.index("boolGameSession::ClockConfigurationCompatible(")]
        self.assertNotIn("DeserializeRecord(", normal)

    def test_clock_metadata_alone_does_not_require_an_index(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        metadata = service[service.index("boolWorldSaveService::save_metadata("):]
        metadata = metadata[:metadata.index("boolWorldSaveService::save_metadata_and_active_regions(")]
        self.assertIn('TransactionScopescope(save_dir,{"world_info.json"},errors);', metadata)
        self.assertLess(metadata.index("if(requires_active_regions&&!has_clock)"),
                        metadata.index("TransactionScopescope("))
        self.assertNotIn("if(requires_active_regions)returnreject(", service)
        self.assertNotIn("if(present||required||format>=1)", service)


class PersistenceAndFieldSourceContract(unittest.TestCase):
    def source(self, relative):
        return compact_cpp((ROOT / relative).read_text(encoding="utf-8"))

    def test_hashing_reads_the_validated_regular_file_handle(self):
        source = self.source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        hashing = source[source.index("boolhash_file("):source.index("booljournal_pending(")]
        self.assertIn("SnapshotInputin(path);", hashing)
        self.assertNotIn("ifstream", hashing)
        self.assertNotIn("fs::exists(", hashing)
        self.assertIn("if(in.missing())returntrue;", hashing)
        self.assertLess(hashing.index("if(!in.valid())"), hashing.index("in.read("))
        self.assertIn("if(!in.read(block.data(),block.size(),count))", hashing)
        self.assertIn("if(count==0)returntrue;", hashing)
        self.assertIn("Fnv1aUpdate(fnv64,block.data(),count)", hashing)
        native = source[source.index("classSnapshotInput{"):source.index("boolReadBoundedBytes(")]
        for guard in ("O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW", "::fstat(m_descriptor,&information)",
                      "S_ISREG(information.st_mode)", "::GetFileType(m_handle)==FILE_TYPE_DISK",
                      "::GetFileInformationByHandle(m_handle,&information)",
                      "FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT",
                      "open_error==ERROR_FILE_NOT_FOUND||open_error==ERROR_PATH_NOT_FOUND",
                      "m_descriptor<0&&errno==ENOENT"):
            self.assertIn(guard, native)

    def test_field_ownership_counts_only_overlapping_page_masks(self):
        source = self.source("src/luminumbra_common/fields/FieldPageState.cpp")
        count = source[source.index("std::size_tFieldPageState::owned_initialized_cells("):
                       source.index("boolFieldPageState::activate_region(")]
        self.assertNotIn("cells_of_region(", count)
        self.assertNotIn("initialized(", count)
        self.assertNotIn(":m_pages)", count)
        for required in ("AxisCellRange(key.x)", "AxisCellRange(key.z)",
                         "if(x.lo>x.hi||z.lo>z.hi)return0;", "m_pages.find(",
                         "FloorDiv(z.lo,kPageSpan)", "FloorDiv(z.hi,kPageSpan)",
                         "FloorDiv(x.lo,kPageSpan)", "FloorDiv(x.hi,kPageSpan)",
                         "std::popcount(", "page->second.mask["):
            self.assertIn(required, count)

    def test_energy_ownership_reads_only_overlapping_pages_without_serialization(self):
        source = self.source("src/luminumbra_common/fields/EnergyFieldState.cpp")
        owns = source[source.index("boolEnergyFieldState::OwnsRegion("):
                      source.index("std::stringEnergyFieldState::RegionBytes(")]
        for forbidden in ("RegionBytes(", "CanonicalBytes(", "ostringstream", ":m_pages)"):
            self.assertNotIn(forbidden, owns)
        for required in ("OwnerOfCell(cx,cz)==owner", "cell_range(owner.x)",
                         "cell_range(owner.z)", "if(x0>=x1||z0>=z1)returnfalse;",
                         "RegionalFloorDiv(z0,kEnergyPageCells)",
                         "RegionalFloorDiv(z1-1,kEnergyPageCells)",
                         "RegionalFloorDiv(x0,kEnergyPageCells)",
                         "RegionalFloorDiv(x1-1,kEnergyPageCells)",
                         "m_pages.find(PackPageKey(",
                         "static_cast<std::uint64_t>(offset+end_x)*"
                         "static_cast<std::uint64_t>(m_channels)",
                         "static_cast<std::uint64_t>(page->second.values.size())",
                         "page->second.values[static_cast<std::size_t>(i)]!=0"):
            self.assertIn(required, owns)

    def test_hash_and_ownership_regressions_are_registered(self):
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        for relative, cases in (
                ("persistence/SaveTransaction_test.cpp",
                 ("HashesMissingEmptyAndBlockBoundaryRegularFiles",
                  "IndexedEnergyFifoIsRefusedWithoutWaitingForAWriter",
                  "HashingRefusesSymlinksWithoutReadingTheirTargets",
                  "HashingRefusesUnreadableAndLockedFiles")),
                ("common/FieldPageState_test.cpp",
                 ("OwnershipCountsOnlyRegionalMaskIntersectionsWithoutChangingBytes",)),
                ("fields/energy_field_state_test.cpp",
                 ("OwnershipQueriesOnlyOverlappingStoredValuesWithoutChangingBytes",))):
            self.assertIn(relative, cmake)
            fixture = self.source("test/" + relative)
            for case in cases:
                self.assertIn("," + case + "){", fixture)
        fifo = self.source("test/persistence/SaveTransaction_test.cpp")
        fifo = fifo[fifo.index(",IndexedEnergyFifoIsRefusedWithoutWaitingForAWriter){"):
                    fifo.index(",HashingRefusesSymlinksWithoutReadingTheirTargets){")]
        self.assertIn("::mkfifo(member.c_str(),0600)", fifo)
        self.assertIn("::alarm(2)", fifo)
        self.assertIn("WorldSaveService::validate_save(save.path(),&errors)", fifo)
        self.assertIn("::testing::ExitedWithCode(0)", fifo)

    def test_undo_protection_reads_each_payload_once_and_indexes_membership(self):
        source = self.source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        protect = source[source.index("boolSaveTransaction::protect("):
                         source.index("boolSaveTransaction::write_journal(")]
        self.assertIn("m_protected_paths.contains(rel)", protect)
        self.assertIn("m_protected_paths.insert(rel);", protect)
        self.assertLess(protect.index("m_entries.size()>=kMaxJournalEntries"),
                        protect.index("ReadAllBytes(destination,previous,errors)"))
        self.assertLess(protect.index("m_entries.size()>=kMaxJournalEntries"),
                        protect.index("m_protected_paths.insert(rel)"))
        self.assertNotIn(":m_entries)", protect)
        self.assertNotIn("hash_file(", protect)
        self.assertEqual(protect.count("ReadAllBytes(destination,previous,errors)"), 1)
        self.assertIn("fs::exists(destination,exists_error)", protect)
        self.assertIn("if(exists_error){AddError(", protect)
        self.assertIn("entry.prev_fnv64=fnv1a64_bytes(previous.data(),previous.size());", protect)
        self.assertLess(protect.index("durable_write_file("), protect.index("write_journal(errors)"))
        read = source[source.index("boolReadAllBytes("):source.index("classSnapshotInput{")]
        self.assertNotIn("istreambuf_iterator", read)
        self.assertIn("std::array<char,std::size_t{64}*1024>block{};", read)
        self.assertIn("in.read(block.data(),static_cast<std::streamsize>(block.size()))", read)
        self.assertLess(read.index("count>out.max_size()-out.size()"),
                        read.index("out.append(block.data(),count)"))
        self.assertIn("if(!in.eof()||in.bad())", read)

    def test_rollback_checks_the_same_undo_bytes_it_restores(self):
        source = self.source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        rollback = source[source.index("boolRollbackEntries("):
                          source.index("voidFinishJournal(")]
        self.assertNotIn("hash_file(undo_path,", rollback)
        read = rollback.index("ReadAllBytes(undo_path,restored,errors)")
        check = rollback.index("restored.size()!=entry.prev_bytes||"
                               "fnv1a64_bytes(restored.data(),restored.size())!=entry.prev_fnv64")
        write = rollback.index("durable_write_file(destination,restored,errors)")
        self.assertLess(read, check)
        self.assertLess(check, write)
        self.assertIn("returnfalse;", rollback[check:write])

    def test_undo_staging_does_not_scan_prior_undo_copies(self):
        source = self.source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        staging = source[source.index("WorldSaveService::region_staging_path("):
                         source.index("boolWorldSaveService::save_metadata(")]
        self.assertIn('if(directory.filename()=="undo"&&'
                      'directory.parent_path().filename()=="journal")'
                      'returndirectory.parent_path()/destination.filename();', staging)
        writer = source[source.index("boolWorldSaveService::durable_write_file("):
                        source.index("boolWorldSaveService::durable_remove(")]
        self.assertIn("constautostaging=region_staging_path(destination);", writer)
        self.assertIn("ReclaimAbandonedStaging(staging);", writer)
        self.assertNotIn("ReclaimAbandonedStaging(destination)", writer)
        self.assertIn("WriteDurableRegionTemp(staging,bytes,temp,errors)", writer)
        self.assertIn("ReplaceMember(temp,destination,errors)", writer)

    def test_first_index_validates_each_protected_member_only_once(self):
        source = self.source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        commit = source[source.index("boolSaveTransaction::commit("):
                        source.index("voidSaveTransaction::abort(")]
        baseline = commit[commit.index("for(conststd::string&rel:baseline)"):
                          commit.index("for(constEntry&entry:m_entries)")]
        self.assertLess(baseline.index("if(m_protected_paths.contains(rel))continue;"),
                        baseline.index("digest_member(rel,digest)"))
        final = commit[commit.index("for(constEntry&entry:m_entries)"):]
        self.assertIn("if(!digest_member(entry.path,digest)){abort(errors);returnfalse;}", final)
        self.assertLess(final.index("digest_member(entry.path,digest)"),
                        final.index("m_state=State::Committing"))

    def test_exact_chunk_reads_share_one_membership_verification(self):
        source = self.source("src/luminumbra_common/persistence/WorldSaveService.cpp")
        read = source[source.index("WorldSaveService::ChunkReadResultWorldSaveService::read_chunk("):
                      source.index("WorldSaveService::chunk_save_acknowledgement(")]
        self.assertEqual(read.count("VerifyRegionMembership(path,records,&errors)"), 1)
        self.assertNotIn("read_snapshot_index(", read)
        self.assertNotIn("Lod0Fingerprint(", read)
        self.assertIn("if(require_index&&!indexed)", read)
        verify = source[source.index("boolVerifyRegionMembership("):
                        source.index("WorldSaveService::SaveOwnership::SaveOwnership(")]
        self.assertEqual(verify.count("read_snapshot_index(save_dir,index,errors)"), 1)
        for check in ("CheckMetadataFile(save_dir,&format)", "format>=1",
                      "member->present!=digest.present", "member->bytes!=digest.bytes",
                      "member->fnv64!=digest.fnv64"):
            self.assertIn(check, verify)

    def test_journal_and_index_reads_stop_at_their_byte_budgets(self):
        source = self.source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        read = source[source.index("boolReadBoundedBytes("):source.index("boolSamePath(")]
        self.assertIn("constautoremaining=limit-out.size();", read)
        self.assertIn("remaining<block.size()?remaining+1:block.size()", read)
        self.assertIn("if(!in.read(block.data(),request,count))", read)
        self.assertIn("out.append(block.data(),count);", read)
        self.assertIn("if(out.size()>limit)", read)
        self.assertIn("if(count==0)returntrue;", read)
        self.assertIn("ReadBoundedBytes(txn_path,kMaxJournalBytes,bytes,errors)", source)
        self.assertIn("ReadBoundedBytes(path,kMaxSnapshotIndexBytes,bytes,errors)", source)
        self.assertNotIn("ReadAllBytes(txn_path,", source)
        native = source[source.index("classSnapshotInput{"):source.index("boolReadBoundedBytes(")]
        for guard in ("O_NONBLOCK", "O_NOFOLLOW", "::fstat(m_descriptor,&information)",
                      "S_ISREG(information.st_mode)", "::GetFileType(m_handle)==FILE_TYPE_DISK",
                      "::GetFileInformationByHandle(m_handle,&information)",
                      "FILE_ATTRIBUTE_REPARSE_POINT", "::ReadFile(m_handle,", "::read(m_descriptor,"):
            self.assertIn(guard, native)

    def test_journal_count_precedes_recovery_state_and_writer_uses_matching_caps(self):
        source = self.source("src/luminumbra_common/persistence/SaveTransaction.cpp")
        parse = source[source.index("boolParseJournal("):source.index("std::stringEncodeJournal(")]
        self.assertLess(parse.index("bytes.size()>kMaxJournalBytes"),
                        parse.index("nlohmann::json::parse("))
        self.assertIn("nlohmann::json::parse(bytes.begin(),bytes.end(),nullptr,false)", parse)
        self.assertNotIn("conststd::stringtext(bytes)", parse)
        self.assertLess(parse.index("entries->size()>kMaxJournalEntries"),
                        parse.index("JournalDocumentparsed;"))
        self.assertLess(parse.index("entries->size()>kMaxJournalEntries"),
                        parse.index("parsed.entries.reserve(entries->size())"))
        writer = source[source.index("boolSaveTransaction::write_journal("):]
        self.assertLess(writer.index("m_entries.size()>kMaxJournalEntries"),
                        writer.index("entries.reserve("))
        self.assertLess(writer.index("bytes.size()>kMaxJournalBytes"),
                        writer.index("WorldSaveService::durable_write_file("))
        header = self.source("src/luminumbra_common/persistence/SaveTransaction.h")
        self.assertIn("kMaxJournalEntries=kMaxSnapshotMembers+1;", header)
        self.assertIn("kMaxJournalBytes=2*kMaxSnapshotIndexBytes;", header)
        session = self.source("src/luminumbra_common/world/GameSession.cpp")
        load = session[session.index("boolGameSession::LoadWorld("):]
        refusal = load[load.index("Persistence::recover_pending_journal("):
                       load.index("Persistence::InspectSavedWorld(")]
        self.assertIn("Persistence::RecoveryOutcome::Failed", refusal)
        self.assertIn("returnfalse;", refusal)

    def test_regional_energy_stages_participating_pages_and_reuses_diffusion_values(self):
        source = self.source("src/luminumbra_common/fields/EnergyFieldState.cpp")
        tick = source[source.index("boolEnergyFieldState::TickRegional("):]
        self.assertIn("EnergyFieldStatestaged(m_channels);", tick)
        self.assertNotIn("staged=*this", tick)
        self.assertIn("for(constautoowner:firing)", tick)
        self.assertIn("RegionalCellRange(owner.x)", tick)
        self.assertIn("if(present(px,pz)&&present(npx,npz))", tick)
        allocation = tick.index("next.emplace(key,std::vector<std::int32_t>(")
        sweep = tick.index("for(intiteration=0;!next.empty()&&iteration<kEnergyDiffuseIterations;")
        self.assertLess(allocation, sweep)
        self.assertIn("constauto&snapshot=staged.m_pages;", tick[sweep:])
        self.assertNotIn("constautosnapshot=staged.m_pages;", tick)
        self.assertNotIn("next.emplace(", tick[sweep:])
        self.assertIn("RegionalRecordFits(&staged)", tick)
        self.assertIn("m_pages.insert(std::move(node))", tick)

    def test_regional_evolution_queries_only_local_scent_and_indexed_weather(self):
        source = self.source("src/luminumbra_common/fields/FieldPageState.cpp")
        visit = source[source.index("boolVisitRegionScent("):source.index("voidAppendOwner(")]
        self.assertIn("if(!ValidOwner(owner)||scent.empty())returnfalse;", visit)
        self.assertIn("kRegionCells=512;", visit)
        self.assertIn("for(autox=min_x;x<end_x;++x)", visit)
        self.assertIn("scent.lower_bound({x,min_z})", visit)
        self.assertIn("cell->first.x==x&&cell->first.z<end_z", visit)
        owns = source[source.index("boolFieldPageState::owns_evolution_state("):
                      source.index("boolFieldPageState::set_weather_age(")]
        projection = source[source.index("std::stringFieldPageState::region_bytes("):]
        for region_query in (owns, projection):
            self.assertIn("VisitRegionScent(m_scent,key,", region_query)
            self.assertNotIn(":m_scent)", region_query)
            self.assertNotIn(":m_storms)", region_query)
            self.assertNotIn(":m_strikes)", region_query)
        self.assertIn("m_ownerStorms.contains(key)", owns)
        self.assertIn("m_ownerStrikes.contains(key)", owns)
        self.assertIn("m_ownerStorms.find(key)", projection)
        self.assertIn("m_ownerStrikes.find(key)", projection)

    def test_refusal_and_working_set_regressions_are_registered(self):
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        for relative, cases in (
                ("persistence/SaveTransaction_test.cpp",
                 ("RecoveryRejectsOversizedJournalWithoutWrites",
                  "RecoveryRejectsTooManyJournalEntriesBeforeEntryDecoding",
                  "RecoveryAcceptsJournalAtByteLimit",
                  "RecoveryRefusesEmptyTruncatedAndNonregularJournalsWithoutWrites",
                  "RecoveryRefusesFifoJournalWithoutWaitingForAWriter",
                  "RecoveryRefusesSymlinkJournalWithoutWrites",
                  "RecoveryRefusesUnreadableAndLockedJournalsWithoutWrites",
                  "FirstIndexDigestsProtectedAndUntouchedMembersOnce",
                  "UndoCopiesAndRestoresEmptyAndBlockBoundaryPayloads",
                  "MissingPartialCorruptAndNonregularUndoCannotBeRestored")),
                ("fields/energy_field_state_test.cpp",
                 ("IdleAndBorderTicksExcludeDormantPagesFromBuffers",
                  "CommonBorderStagesBothExistingPhysicalPagesAndSkipsAbsentPages",
                  "DepositOnlyTickStagesOnlyItsTargetAndRetainsLegacyEpochs",
                  "NegativeCommonBordersStageBothAxesInCanonicalOrder",
                  "IdleTickDropsLegacyEmptyPagesAndRejectsInvalidMetadataAtomically")),
                ("common/FieldPageState_test.cpp",
                 ("ScentRegionQueriesMatchCanonicalProjectionAtEveryBoundary",
                  "ScentRegionQueriesExcludeOtherColumnsAtFullCapacity",
                  "WeatherOwnerIndicesSurviveParseCopyRefusalReplacementAndClear"))):
            self.assertIn(relative, cmake)
            fixture = self.source("test/" + relative)
            for case in cases:
                self.assertIn("," + case + "){", fixture)


class SaveWorkSourceContract(unittest.TestCase):
    def source(self, path):
        return compact_cpp((ROOT / path).read_text(encoding="utf-8"))

    def service(self):
        return self.source("src/luminumbra_common/persistence/WorldSaveService.cpp")

    def test_commit_reuses_exact_record_validation_with_bounded_storage(self):
        source = self.service()
        digest = source[source.index("boolWorldSaveService::member_digest("):
                        source.index("boolWorldSaveService::write_member_bytes(")]
        self.assertIn("cache->chunks.find(key)", digest)
        self.assertIn("validated=cached->second;", digest)
        self.assertIn("DecodeChunkRecord(record,path,&diagnostics)", digest)
        self.assertIn("record.id,record.flags,record.uncompressed_size,record.compressed_payload", digest)
        self.assertIn("validated.sdf_size!=0&&validated.sdf_size!=kLattice", digest)
        cache = source[source.index("voidRememberValidatedChunk("):
                       source.index("boolReplaceMember(")]
        for guard in ("kMaxPayloadBytes=64u*1024u*1024u", "kMaxEntries=4096",
                      "kMaxPayloadBytes-cache->payload_bytes", "cache->chunks.size()>=kMaxEntries"):
            self.assertIn(guard, cache)
        self.assertIn("cache->chunks.lower_bound(RecordValidationView{record.id,0,0,{}})", cache)
        self.assertIn("previous=cache->chunks.erase(previous)", cache)
        self.assertLess(cache.index("cache->chunks.erase(previous)"), cache.index("kMaxPayloadBytes"))
        self.assertIn("RememberValidatedChunk(record,*chunks.front());", source)
        self.assertIn("RecordValidationCaches().erase(this);ActiveFacts()=m_previous;", source)
        # Commit still reads installed bytes and checks region identity and unknown far tiers.
        for check in ("ReadRegionFile(path,records,&diagnostics,{},true)",
                      "chunk_rx!=rx||chunk_rz!=rz", "FarLodStore::decode_payload("):
            self.assertIn(check, digest)

    def test_region_copy_batches_reads_and_replacements_and_refuses_partial_authority(self):
        source = self.service()
        copy = source[source.index("boolWorldSaveService::copy_authoritative_chunks("):
                      source.index("WorldSaveService::ChunkReadResultWorldSaveService::read_chunk(")]
        self.assertEqual(copy.count("read_snapshot_index("), 1)
        region_loop = copy.index("for(constauto&[key,ids]:regions){constautorelative=")
        self.assertLess(copy.index("read_snapshot_index("), region_loop)
        self.assertIn("std::map<std::pair<int,int>,std::set<ChunkID>>regions", copy)
        self.assertNotIn("read_chunk(", copy)
        self.assertEqual(copy.count("WriteRegionFile("), 1)
        for refusal in ("if(inventory.empty())returntrue;", "SameSaveDirectory(source,destination)",
                        "if(index.generation==0)", "Chunk::decode_id(id)!=coords", "member==indexed.end()",
                        "member->second.bytes!=digest.bytes", "member->second.fnv64!=digest.fnv64",
                        "if(!ReadRegionFile(source_path,records,errors))returnfalse;",
                        "copied!=ids.size()", "!chunk->sdf_data.empty()&&chunk->sdf_data.size()!=kLattice",
                        "chunk->get_coords()!=Chunk::decode_id(record.id)",
                        "!ReadRegionFile(target_path,target,errors,{},true)"):
            self.assertIn(refusal, copy)
        self.assertLess(copy.index("TransactionScopescope("), copy.index("WriteRegionFile("))
        self.assertLess(copy.index("validate_save(destination,errors)"), copy.index("WriteRegionFile("))
        self.assertIn("scope.commit(errors)", copy)
        session = self.source("src/luminumbra_common/world/GameSession.cpp")
        save = session[session.index("boolGameSession::SaveWorldStateToImpl("):
                       session.index("boolGameSession::LoadWorldState()")]
        self.assertIn("copy_authoritative_chunks(m_regionDurableDirectory,save_dir,missing,&copy_errors)", save)
        self.assertNotIn("WorldSaveService::read_chunk(", save)
        self.assertIn("if(!state.find_chunk(Chunk::calculate_id(coords)))missing.push_back(coords);", save)
        # Relocation writes clean live/parked chunks too, including into existing destinations.
        self.assertIn("constboolrelocating_authority=m_activeRegionsEnabled&&"
                      "!m_regionDurableDirectory.empty()&&source_directory!=target_directory&&!same_existing_directory;", save)
        self.assertIn("constboolwrite_chunks=full_snapshot||relocating_authority||!dirty_ids.empty();", save)
        self.assertIn("if(full_snapshot||relocating_authority||!has_snapshot)"
                      "ok=service.save_world(state,save_dir,&errors);", save)

    def test_legacy_complete_save_shares_validation_across_all_member_writers(self):
        session = self.source("src/luminumbra_common/world/GameSession.cpp")
        save = session[session.index("boolGameSession::SaveWorldStateToImpl("):
                       session.index("boolGameSession::LoadWorldState()")]
        self.assertIn("std::optional<Persistence::WorldSaveService::SaveFactsScope>legacy_facts;", save)
        self.assertIn("!Persistence::WorldSaveService::SaveFactsScope::active_for(save_dir)", save)
        self.assertLess(save.index("legacy_facts.emplace(save_dir)"), save.index("validate_save(save_dir)"))
        self.assertLess(save.index("legacy_facts.emplace(save_dir)"), save.index("service.save_world("))
        # The shared baseline cannot hide invalid newly serialized chunks or plants.
        source = self.service()
        chunks = source[source.index("boolWorldSaveService::write_snapshot("):]
        self.assertLess(chunks.index("if(ActiveRecordValidationCache()&&!finite_values)"),
                        chunks.index("CompressPayload(payload,record,errors)"))
        invalid = chunks[chunks.index("if(ActiveRecordValidationCache()&&!finite_values)"):
                         chunks.index("CompressPayload(payload,record,errors)")]
        self.assertIn("returnfalse;", invalid)
        plants = source[source.index("boolWorldSaveService::save_plant_entities("):
                        source.index("boolWorldSaveService::load_plant_entities(")]
        self.assertIn("if(ActiveFacts())", plants)
        self.assertIn("if(!Luminumbra::Ecs::LoadEntityRegistrySnapshotJson(bytes,checked,payload_errors))", plants)
        self.assertLess(plants.index("LoadEntityRegistrySnapshotJson("), plants.index("ReclaimAbandonedStaging("))
        self.assertIn("returnfalse;", plants[plants.index("LoadEntityRegistrySnapshotJson("):
                                           plants.index("ReclaimAbandonedStaging(")])

    def test_save_identity_uses_existing_directory_identity_in_every_sibling(self):
        for filename, start, prefix in (
            ("src/luminumbra_common/persistence/WorldSaveService.cpp", "boolSameSaveDirectory(", "std::filesystem"),
            ("src/luminumbra_common/persistence/SaveTransaction.cpp", "boolSamePath(", "fs")):
            source = self.source(filename)
            identity = source[source.index(start):]
            self.assertLess(identity.index("if(" + prefix + "::equivalent(lhs,rhs,ec))returntrue;"),
                            identity.index(prefix + "::weakly_canonical(lhs,ec)"))
            self.assertIn("ec.clear();", identity[:identity.index(prefix + "::weakly_canonical(lhs,ec)")])
        session = self.source("src/luminumbra_common/world/GameSession.cpp")
        save = session[session.index("boolGameSession::SaveWorldStateToImpl("):
                       session.index("boolGameSession::LoadWorldState()")]
        self.assertIn("constboolcanonical_save=SameSaveDestination(save_dir,GetWorldSaveDir());", save)
        self.assertIn("SameSaveDestination(m_regionDurableDirectory,save_dir)", save)
        self.assertIn("source_directory!=target_directory&&!same_existing_directory", save)
        self.assertIn("lhs==rhs||(!lhs.empty()&&!rhs.empty()&&fs::equivalent(lhs,rhs,identity_error))", session)
        self.assertIn("if(SameSaveDestination(save_dir,GetWorldSaveDir()))", session)

    def test_validation_refresh_rereads_installed_bytes_without_materializing_cached_chunks(self):
        service = self.service()
        public = service[service.index("boolWorldSaveService::load_world("):
                         service.index("boolWorldSaveService::load_world_impl(")]
        self.assertIn("retain_chunk,authoritative_inventory,nullptr", public)
        validation = service[service.index("boolWorldSaveService::load_world_impl("):
                             service.index("std::stringWorldSaveService::world_hash(")]
        self.assertIn("validation_chunks?ActiveRecordValidationCache():nullptr", validation)
        self.assertIn("cache->chunks.find(proof)", validation)
        self.assertIn("record.id,record.flags,record.uncompressed_size,record.compressed_payload", validation)
        self.assertIn("ReadRegionFile(path,records,&errors,stop)", validation)
        self.assertIn("DecodeChunkRecord(record,path,&errors)", validation)
        for check in ("validated.sdf_size!=kLattice", "chunk_rx!=rx||chunk_rz!=rz",
                      "!decoded_ids.insert(record.id).second", "*validation_chunks=false;",
                      "if(validation_chunks){*validation_chunks=true;continue;}"):
            self.assertIn(check, validation)
        facts = service[service.index("boolWorldSaveService::SaveFactsScope::validate("):
                        service.index("voidWorldSaveService::SaveFactsScope::defer_until_commit(")]
        self.assertIn("if(refresh)m_validated=false;", facts)
        self.assertIn("load_world_impl(state,m_save_dir,m_diagnostics,{},{},nullptr,&m_chunk_snapshot_present)", facts)
        session = self.source("src/luminumbra_common/world/GameSession.cpp")
        save = session[session.index("boolGameSession::SaveWorldStateToImpl("):
                       session.index("boolGameSession::LoadWorldState()")]
        self.assertIn("if(ok&&legacy_facts&&write_chunks)ok=legacy_facts->validate(&errors,true);", save)
        refresh = save.rindex("legacy_facts->validate(&errors,true)")
        self.assertLess(save.index("SaveEnergyFieldRecord(save_dir)"), refresh)
        self.assertLess(refresh, save.index("write_metadata?SaveWorldMetadataTo(save_dir)"))
        self.assertLess(refresh, save.index("acknowledge();"))

    def test_water_authority_skips_unchanged_epochs_and_handles_saturation_and_adoption(self):
        chunk = self.source("src/luminumbra_common/world/Chunk.h")
        dirty = chunk[chunk.index("voidmark_water_state_dirty()"):chunk.index("boolis_water_state_dirty()")]
        self.assertLess(dirty.index("m_water_state_dirty.store(true"), dirty.index("NoteWaterMutation();"))
        implementation = self.source("src/luminumbra_common/world/Chunk.cpp")
        self.assertIn("previous!=std::numeric_limits<std::uint64_t>::max()", implementation)
        self.assertIn("epoch.compare_exchange_weak(previous,previous+1u", implementation)
        world = self.source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        sync = world[world.index("voidSHIELD_WorldSystem::sync_water_authority()"):
                     world.index("boolSHIELD_WorldSystem::adopt_streamed_chunk(")]
        self.assertIn("constautoepoch=Chunk::water_mutation_epoch();", sync)
        self.assertIn("epoch!=std::numeric_limits<std::uint64_t>::max()&&m_water_authority_epoch==epoch", sync)
        self.assertLess(sync.index("m_water_authority_epoch==epoch"), sync.index(":m_streaming_state.chunks)"))
        self.assertLess(sync.index(":m_parked)"), sync.index("m_water_authority_epoch=epoch;"))
        for begin, end in (("voidSHIELD_WorldSystem::clear_world(", "voidSHIELD_WorldSystem::regenerate_all_chunks("),
                           ("boolSHIELD_WorldSystem::unpark_chunk(", "voidSHIELD_WorldSystem::sync_water_authority()")):
            self.assertIn("m_water_authority_epoch.reset();", world[world.index(begin):world.index(end)])
        self.assertIn("if(adopted){m_water_authority_epoch.reset();", world)

    def test_region_reads_use_one_regular_handle_on_both_platforms(self):
        source = self.service()
        native = source[source.index("classRegionInput{"):source.index("boolReadRegionFile(")]
        for check in ("O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW", "::fstat(m_descriptor,&information)",
                      "S_ISREG(information.st_mode)", "::GetFileType(m_handle)==FILE_TYPE_DISK",
                      "::GetFileInformationByHandle(m_handle,&information)",
                      "FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT",
                      "open_error==ERROR_FILE_NOT_FOUND||open_error==ERROR_PATH_NOT_FOUND",
                      "m_descriptor<0&&errno==ENOENT"):
            self.assertIn(check, native)
        reader = source[source.index("boolReadRegionFile("):source.index("boolWriteRegionFile(")]
        self.assertIn("if(input.missing()&&allow_missing)returntrue;", reader)
        self.assertIn("if(!input.valid())", reader)
        self.assertIn("stop.stop_requested()", reader)
        self.assertIn("std::min(size,std::size_t{64}*1024)", reader)
        for bound in ("compressed_size>remaining", "compressed_size>kMaxRecordBytes",
                      "record.uncompressed_size>kMaxRecordBytes"):
            self.assertLess(reader.index(bound), reader.index("record.compressed_payload.resize("))
        self.assertIn("remaining!=0||!input.read(&extra,1,count)||count!=0", reader)
        self.assertNotIn("ifstream", reader)
        self.assertNotIn("std::filesystem::exists(", reader)

    def test_fresh_snapshot_proof_checks_every_float_before_reusing_serialization(self):
        source = self.source("src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp")
        finite = source[source.index("boolHasOnlyFiniteFloats("):source.index("std::stringStableDump(")]
        for check in ("value.is_number_float()", "std::isfinite(value.get<double>())",
                      "value.is_structured()", "for(constauto&child:value)", "!HasOnlyFiniteFloats(child)"):
            self.assertIn(check, finite)
        serializer = source[source.index("std::stringSerializeWorldStreamingStateSnapshotJson("):
                            source.index("boolLoadWorldStreamingStateSnapshotJson(")]
        self.assertIn("if(finite_values)*finite_values=HasOnlyFiniteFloats(snapshot);", serializer)
        dump = source[source.index("std::stringStableDump("):source.index("std::stringChecksum(")]
        self.assertLess(dump.index("if(!stream)throwstd::runtime_error("), dump.index("returnstream.str();"))
        service = self.service()
        write = service[service.index("boolWorldSaveService::write_snapshot("):]
        self.assertIn("boolfinite_values=false;", write)
        self.assertIn("ActiveRecordValidationCache()?&finite_values:nullptr", write)
        self.assertIn("if(finite_values)RememberValidatedChunk(record,*chunk);", write)
        self.assertLess(write.index("CompressPayload(payload,record,errors)"),
                        write.index("RememberValidatedChunk(record,*chunk)"))
        region = service[service.index("boolWriteRegionFile("):
                         service.index("std::stringSerializeSingleChunkSnapshot(")]
        self.assertLess(region.index("bytes.max_size()-total_bytes"), region.index("bytes.reserve(total_bytes)"))
        self.assertIn("std::numeric_limits<std::uint32_t>::max()", region)
        compression = service[service.index("boolCompressPayload("):service.index("boolDecompressPayload(")]
        self.assertLess(compression.index("payload.empty()||payload.size()>kMaxRecordBytes"),
                        compression.index("LZ4_compressBound("))
        self.assertIn("written<=0||static_cast<std::size_t>(written)>kMaxRecordBytes", compression)

    def test_scheduler_and_world_hash_partition_chunk_inputs_once_per_invocation(self):
        source = self.source("src/luminumbra_common/world/GameSession.cpp")
        group = source[source.index("RegionalChunkInputsGroupRegionChunks("):
                       source.index("structGameSession::LoadedWorldState")]
        for destination in (".live.push_back(chunk)", ".parked.push_back(chunk)", ".durable.push_back(coords)"):
            self.assertIn(destination, group)
        schedule = source[source.index("voidGameSession::RunRegionScheduler("):
                          source.index("std::vector<RegionKey>GameSession::SortedDueRegions(")]
        self.assertIn("if(!digest_inputs)digest_inputs=GroupRegionChunks(", schedule)
        self.assertIn("auto&inputs=(*digest_inputs)[key];", schedule)
        self.assertIn("region_simulation_digest(directory,key,inputs)", schedule)
        hashing = source[source.index("std::stringGameSession::ComputeChunkStateHash()"):
                         source.index("voidGameSession::RestoreWorldClock(")]
        self.assertIn("constautogrouped=GroupRegionChunks(live,parked,durable);", hashing)
        self.assertIn("grouped.at(key)", hashing)
        self.assertIn("?record->second.frozen_digest", hashing)

    def test_energy_admission_reuses_immutable_page_facts_and_invalidates_every_writer(self):
        source = self.source("src/luminumbra_common/fields/EnergyFieldState.cpp")
        facts = source[source.index("EnergyFieldState::Page::regional_facts("):
                       source.index("std::stringEnergyFieldState::RegionBytes(")]
        self.assertLess(facts.index("if(regional_summary)return*regional_summary;"),
                        facts.index("i<values.size()"))
        for guard in ("channels>=1&&channels<=64", "px>=-43691&&px<=43690",
                      "pz>=-43691&&pz<=43690", "if(!ValidEnergyOwner(owner))",
                      "facts.owner_count==facts.owners.size()", "facts.record_bytes>kMaxRegionalRecordBytes",
                      "token_bytes>kMaxRegionalRecordBytes-facts.record_bytes"):
            self.assertIn(guard, facts)
        self.assertIn("regional_summary=facts;return*regional_summary;", facts)
        for begin, end in (("EnergyFieldState::Page&EnergyFieldState::EnsurePage(",
                            "voidEnergyFieldState::CatchUpWindowPages()"),
                           ("voidEnergyFieldState::CatchUpWindowPages()",
                            "std::uint64_tEnergyFieldState::ApplyDeposits()"),
                           ("voidEnergyFieldState::DecayAndDiffuseWindow()",
                            "voidEnergyFieldState::StampWindowPages()"),
                           ("voidEnergyFieldState::NormalizeAllPages()",
                            "std::stringEnergyFieldState::SerializeRecord(")):
            self.assertIn("regional_summary.reset();", source[source.index(begin):source.index(end)])
        tick = source[source.index("boolEnergyFieldState::TickRegional("):]
        stage = tick[tick.index("constautostage_page="):tick.index("std::set<std::pair<RegionKey,std::int64_t>>touched;")]
        self.assertIn("existing->second.regional_summary.reset();", stage)
        self.assertIn("page.regional_summary.reset();", stage)
        owners = source[source.index("boolEnergyFieldState::ValidateRegionalOwners("):
                        source.index("boolEnergyFieldState::TickRegional(")]
        self.assertIn("page.regional_facts(key,m_channels)", owners)
        self.assertIn("if(!facts.valid)returnfalse;", owners)
        self.assertIn("!records.contains(facts.owners[i])||!ages.contains(facts.owners[i])", owners)
        self.assertNotIn("page.values", owners)
        capacity = source[source.index("boolEnergyFieldState::RegionalRecordFits("):
                          source.index("boolEnergyFieldState::DeserializeRegionalRecord(")]
        self.assertIn("page.regional_facts(key,m_channels).record_bytes", capacity)
        self.assertIn("count>kMaxRegionalRecordBytes-bytes", capacity)
        self.assertNotIn("page.values", capacity)

    def test_energy_region_hash_projects_at_most_nine_pages_in_canonical_order(self):
        source = self.source("src/luminumbra_common/fields/EnergyFieldState.cpp")
        region = source[source.index("std::stringEnergyFieldState::RegionBytes("):
                        source.index("std::stringEnergyFieldState::CanonicalRegionalBytes()")]
        for required in ("if(!ValidEnergyOwner(owner))return{};", "std::array<std::int64_t,9>keys{};",
                         "RegionalCellRange(owner.x)", "RegionalCellRange(owner.z)",
                         "FloorDiv(end_x-1,kEnergyPageCells)", "FloorDiv(end_z-1,kEnergyPageCells)",
                         "std::sort(keys.begin(),keys.begin()+static_cast<std::ptrdiff_t>(key_count))",
                         "constautofound=m_pages.find(key);", "if(found==m_pages.end())continue;",
                         "OwnerOfCell(static_cast<int>(cx),static_cast<int>(cz))!=owner"):
            self.assertIn(required, region)
        self.assertNotIn(":m_pages)", region)

    def test_load_from_uses_staged_energy_and_merge_retains_both_simulation_gates(self):
        session = self.source("src/luminumbra_common/world/GameSession.cpp")
        load = session[session.index("boolGameSession::LoadWorldFrom("):
                       session.index("boolGameSession::ClockConfigurationCompatible(")]
        self.assertNotIn("DeserializeRecord(", load)
        self.assertLess(load.index("ReadWorldStateFrom(worldPath,loaded)"), load.index("ResetWorldSystems();"))
        self.assertIn("AdoptWorldStateFrom(worldPath,loaded);m_worldUsable=true;", load)
        client = self.source("src/luminumbra_client/main_client.cpp")
        self.assertIn("if(!settling_benchmark_uploads){if(simulate_frame)", client)
        self.assertIn("if(advance_frame_simulation){if(auto*physics=gameSession->GetPhysicsSystem();physics&&!settling_benchmark_uploads)", client)
        self.assertIn("advance_frame_simulation);streaming_world->SetWaterBootPaused(false);", client)
        world = self.source("src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        self.assertIn("booladvance_simulation){if(m_durable_read_failed)return;", world)


if __name__ == "__main__":
    unittest.main()
