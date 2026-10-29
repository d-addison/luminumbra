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
                      "source,destination,original,serialized)", writer)
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        rename = service[service.index("boolWorldSaveService::rename_file_durably("):]
        rename = rename[:rename.index("boolWorldSaveService::remove_file_and_staging(")]
        self.assertIn("{constSaveGuardguard;", rename)
        self.assertLess(rename.index("contents!=expected_bytes"),
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
        self.assertIn("Detail::WindowsPublishWithoutReplacement(std::exchange(m_handle,INVALID_HANDLE_VALUE),"
                      "m_temp,m_destination,GENERIC_WRITE|DELETE|FILE_READ_ATTRIBUTES)", publish)
        self.assertIn("::FlushFileBuffers(m_handle)", publish)
        journal = compact_cpp((ROOT / "src/luminumbra_common/persistence/CreatedWorldArtifacts.h")
                              .read_text(encoding="utf-8"))
        self.assertIn("::CreateHardLinkW(bound_destination.c_str(),original_name.c_str(),nullptr)",
                      journal)
        self.assertIn("link->ReplaceIfExists=FALSE", journal)
        self.assertIn("WindowsVacantPublicationNameError(bound_destination)", journal)
        self.assertIn("WindowsPublicationLinkCount(original.get())!=2", journal)
        self.assertIn("WindowsPublicationLinkCount(linked.get())!=1", journal)
        self.assertNotIn("FileRenameInfo", journal)
        self.assertNotIn("::MoveFileExW(", publish)
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

    def test_explicit_config_reader_checks_original_bytes_before_replacing_snapshot(self):
        source = compact_cpp((ROOT / "src/luminumbra_common/core/SystemConfig.cpp")
                             .read_text(encoding="utf-8"))
        reader = source.split("boolSystemConfig::TryLoadFromFile(", 1)[1].split(
            "voidSystemConfig::ReplaceSystemsFrom(", 1)[0]
        self.assertIn("std::ifstreamfile(path,std::ios::binary);", reader)
        self.assertIn("constautobytes=buffer.str();", reader)
        self.assertIn("CheckSettingsBytes(bytes);", reader)
        self.assertIn("constautodocument=nlohmann::json::parse(bytes);", reader)
        self.assertLess(reader.index("CheckSettingsBytes(bytes)"),
                        reader.index("nlohmann::json::parse(bytes)"))
        self.assertLess(reader.index("nlohmann::json::parse(bytes)"),
                        reader.index("config.ReplaceSystemsFrom("))
        guard = source.split("voidCheckSettingsBytes(", 1)[1].split(
            "nlohmann::jsonParseSettingsJson(", 1)[0]
        self.assertIn("json_text.find('\\0')", guard)
        self.assertIn("if(nul!=std::string::npos)throw", guard)
        parser = source.split("nlohmann::jsonParseSettingsJson(", 1)[1].split(
            "voidParseUserSection(", 1)[0]
        self.assertLess(parser.index("CheckSettingsBytes(json_text);"),
                        parser.index("nlohmann::json::parse(text)"))

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
        self.assertIn("std::ifstreamfile(path,std::ios::binary);", source)
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
        self.assertLess(self.client[start:end].index("ReadSavedJson("),
                        self.client[start:end].index("Writer::write_file_durably("))
        settings = compact_cpp((ROOT / "src/luminumbra_common/core/SystemConfig.cpp")
                               .read_text(encoding="utf-8"))
        save = settings[settings.index("boolSystemConfig::SaveUserOverlay("):]
        self.assertIn("WorldSaveService::write_file_durably(path,serialized)", save)

    def test_saved_preset_consumers_share_admission_before_recursive_work(self):
        for start_name, end_name, output in (
                ("g_uiManager->SetWorldParamGetter(", "g_uiManager->SetWorldPresetSaver(",
                 "ResolveTerrainPresetDefaults(j)"),
                ("g_uiManager->SetWorldPresetSaver(", "g_uiManager->SetWorldPresetList(",
                 "BuildKnobResolvedPreset(base,params)"),
                ("g_uiManager->SetWorldPresetList(", "g_uiManager->SetWorldPresetExists(",
                 'j.value("name",stem)')):
            start = self.client.index(start_name)
            body = self.client[start:self.client.index(end_name, start)]
            self.assertLess(body.index("ReadSavedJson("), body.index(output))
            self.assertIn("SavedInputLimits::kPresetBytes", body)
            self.assertNotIn("std::ifstream", body)
        start = self.client.index("g_uiManager->SetWorldPresetSaver(")
        saver = self.client[start:self.client.index("g_uiManager->SetWorldPresetList(", start)]
        self.assertLess(saver.index("displayName.size()>"), saver.index('saved["name"]=displayName'))
        self.assertLess(saver.index("BoundedJsonDump("), saver.index("Writer::write_file_durably("))
        self.assertIn("nlohmann::json::parse(saved.preset_json)", self.client)
        self.assertNotIn("std::ifstreaminput(saved.preset_path", self.client)
        menu = compact_cpp((ROOT / "src/luminumbra_client/app/MenuScreens.cpp").read_text())
        self.assertLess(menu.index("ReadSavedJson("), menu.index("BuildKnobResolvedPreset(base,"))
        preset = compact_cpp((ROOT / "src/luminumbra_client/world/WorldgenOverride.cpp").read_text())
        rename = preset[preset.index("boolRenameUserPreset("):preset.index("CustomPresetResultBuildCustomPreset(")]
        self.assertLess(rename.index("display_name.size()>"), rename.index('preset["name"]=display_name'))
        self.assertLess(rename.index("JsonDepthAllowed(original)"), rename.index("json::parse(original)"))
        self.assertLess(rename.index("BoundedJsonDump("), rename.index("rename_file_durably("))
        admission = self.admission[:self.admission.index("HelloMsgRuntimeHelloFrom(")]
        self.assertLess(admission.index("JsonDepthAllowed(bytes)"), admission.index("json::parse(bytes"))
        self.assertIn("ReadSavedJson(", admission)
        self.assertEqual(self.admission.count("!ReadPresetInput(options,"), 2)
        trace = compact_cpp((ROOT / "src/luminumbra_common/replay/RegionScheduleTrace.cpp").read_text())
        self.assertIn("ReadSavedJson(path,kMaxTraceBytes,value,missing,false)", trace)
        self.assertLess(trace.index("BoundedJsonDump("), trace.index("write_file_durably(path,bytes)"))

    def test_overlay_failure_regression_reaches_the_owned_staging_writer(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        start = service.index("boolWriteFileDurably(", service.index("boolWriteFileDurably(") + 1)
        write = service[start:service.index("boolWorldSaveService::write_file_durably(", start)]
        interrupt = "InterruptBeforeRegionReplaceForTesting().exchange(false,std::memory_order_acq_rel)"
        self.assertLess(write.index("WriteDurableRegionTemp(staging,bytes,temp_path,errors,artifacts)"),
                        write.index(interrupt))
        self.assertLess(write.index(interrupt), write.index("ReplaceRegionFileAtomically("))
        interrupted = write[write.index(interrupt):write.index("ReplaceRegionFileAtomically(")]
        self.assertIn("RemoveTemporaryFile(temp_path,artifacts);", interrupted)
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

    def test_ledger_staging_requires_persistent_matching_ownership(self):
        source = (ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp").read_text(encoding="utf-8")
        service = compact_cpp(source)
        cleanup = service[service.index("std::size_tReclaimAbandonedStaging("):]
        cleanup = cleanup[:cleanup.index("#ifdefined(_WIN32)")]
        self.assertIn('if(StagingNameEquals(destination.filename(),"active-regions.arl"))', cleanup)
        self.assertIn('elseif(ReclaimOwnedLedgerStaging(entry.path()))', cleanup)
        self.assertLess(cleanup.index('StagingNameEquals(destination.filename(),"active-regions.arl")'),
                        cleanup.index('entry.is_regular_file(kind_error)'))
        owned = service[service.index("boolReclaimOwnedLedgerStaging("):service.index("std::filesystem::pathNextLedgerTempPath(")]
        self.assertLess(owned.index("ValidStagingOwnership(staging,payload,record)"),
                        owned.index("payload.remove()"))
        self.assertLess(owned.index("payload.remove()"), owned.index("record.remove()"))
        validator = service[service.index("boolValidStagingOwnership("):service.index("boolReclaimOwnedLedgerStaging(")]
        for proof in ("payload.matches_name()", "record.matches_name()",
                      "directory.identity(world_identity,true)", "payload.identity(staging_identity)",
                      "record.identity(instance_identity)", "record.read_record(recorded)"):
            self.assertIn(proof, validator)
        self.assertIn("if(expected.empty())returnrefuse(", validator)
        self.assertIn("if(recorded!=expected)returnrefuse(", validator)
        encoder = service[service.index("std::stringStagingOwnershipBytes("):service.index("boolValidStagingOwnership(")]
        for field in ("world_identity[index]", "staging_identity", "instance_identity[index]",
                      "bytes+=name;", "bytes+=instance;", "AppendU64(bytes,checksum)"):
            self.assertIn(field, encoder)
        self.assertIn('"AROWN001"', encoder)
        native = service[service.index("classStagingFile{"):service.index("std::filesystem::pathOwnershipPath(")]
        for required in ("O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC", "::fstat(m_descriptor,&info)",
                         "FILE_FLAG_OPEN_REPARSE_POINT", "::GetFileInformationByHandle(m_handle,&info)",
                         "FILE_DISPOSITION_INFO", "::lstat(m_path.c_str(),&named)",
                         "current[5]>512", "::FlushFileBuffers(m_handle)", "::fsync(m_descriptor)"):
            self.assertIn(required, native)
        self.assertLess(native.index("current[5]>512"), native.index("bytes.assign("))

    def test_ledger_orphan_cleanup_validates_proof_and_absence_before_verified_removal(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        cleanup = service[service.index("std::size_tReclaimAbandonedStaging("):]
        self.assertIn('if(StagingNameEquals(entry.path().extension(),".owner"))', cleanup)
        self.assertIn('staging.replace_extension();if(ReclaimOrphanedLedgerStaging(staging))', cleanup)
        validator = service[service.index("boolValidOrphanedStagingOwnership("):
                            service.index("boolStagingPathMissing(")]
        for proof in ("record.matches_name()", "directory.identity(world_identity,true)",
                      "record.identity(instance_identity)", "record.read_record(recorded)",
                      "StagingOwnershipBytes(staging,world_identity,staging_identity,instance_identity)",
                      "if(expected.empty()||recorded!=expected)returnfalse;"):
            self.assertIn(proof, validator)
        self.assertLess(validator.index('recorded.size()<'), validator.index('ReadU64('))
        missing = service[service.index("boolStagingPathMissing("):
                          service.index("boolReclaimOrphanedLedgerStaging(")]
        self.assertIn('error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND', missing)
        self.assertIn('::lstat(staging.c_str(),&named)==0', missing)
        self.assertIn('if(errno==ENOENT)returntrue;', missing)
        orphan = service[service.index("boolReclaimOrphanedLedgerStaging("):
                         service.index("boolReclaimClaimedLedgerStaging(")]
        ordered = ("ValidOrphanedStagingOwnership(staging,record)",
                   "StagingPathMissing(staging)", "record.remove()", "FlushRemovalDirectory(")
        positions = [orphan.index(value) for value in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn('payload.remove()', orphan)
        fixture = compact_cpp((ROOT / "test/persistence/world_save_service_test.cpp")
                              .read_text(encoding="utf-8"))
        for regression in ("LedgerStagingReclaimsValidOrphanAfterPublication",
                           "LedgerStagingPreservesInvalidOrphanOwnership",
                           "LedgerStagingRepeatedPublicationInterruptionsDoNotAccumulateSidecars",
                           "LedgerStagingReclaimsValidOrphanAfterPayloadRemoval",
                           "LedgerStagingPreservesOrphanCopiedFromAnotherWorldOrInstance"):
            self.assertIn('TEST(WorldSaveService,' + regression + ')', fixture)

    def test_staging_directory_identity_excludes_mutable_stamps_on_both_platforms(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        identity = service[service.index("boolidentity(StagingIdentity&result,"):
                           service.index("boolwrite(conststd::string&bytes)")]
        windows, posix = identity.split("#else", 1)
        for implementation, stable, stamp in (
                (windows, "result={info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,0,0,0};",
                 "info.ftCreationTime"),
                (posix, "result={static_cast<std::uint64_t>(info.st_dev),"
                 "static_cast<std::uint64_t>(info.st_ino),0,0,0,0};", "info.st_ctimespec")):
            self.assertIn(stable + "if(directory)returntrue;", implementation)
            self.assertLess(implementation.index("if(directory)returntrue;"),
                            implementation.index(stamp))
            for field in ("result[3]=", "result[4]=", "result[5]="):
                self.assertIn(field, implementation)
        self.assertIn("info.st_ctim;", posix)
        encoder = service[service.index("std::stringStagingOwnershipBytes("):
                          service.index("boolValidStagingOwnership(")]
        # No format change: 3 directory, 6 payload and 3 sidecar identity words.
        self.assertIn('std::stringbytes="AROWN001";', encoder)
        self.assertIn('index<3;++index)AppendU64(bytes,world_identity[index]);', encoder)
        self.assertIn('for(constautovalue:staging_identity)AppendU64(bytes,value);', encoder)
        self.assertIn('index<3;++index)AppendU64(bytes,instance_identity[index]);', encoder)
        self.assertIn('native_name.size()>256', encoder)
        orphan = service[service.index("boolValidOrphanedStagingOwnership("):
                         service.index("boolStagingPathMissing(")]
        self.assertIn('kPayloadIdentityOffset=8+3*8;', orphan)
        self.assertLess(orphan.index('recorded.size()<kPayloadIdentityOffset+staging_identity.size()*8'),
                        orphan.index('ReadU64('))

    def test_windows_staging_absence_opens_leaf_without_following_reparse_points(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        missing = service[service.index("boolStagingPathMissing("):
                          service.index("boolReclaimOrphanedLedgerStaging(")]
        windows, posix = missing.split("#ifdefined(_WIN32)", 1)[1].split("#else", 1)
        self.assertIn('constHANDLEleaf=::CreateFileW(staging.c_str(),FILE_READ_ATTRIBUTES,'
                      'FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,'
                      'FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);', windows)
        ordered = ('if(leaf!=INVALID_HANDLE_VALUE){', 'if(!::CloseHandle(leaf))',
                   'cannotclosestagingleafinspectionhandleduringorphancleanup',
                   'preservingstagingownership:payloadleafisstillpresent',
                   'constDWORDerror=::GetLastError();',
                   'if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)returntrue;')
        positions = [windows.index(value) for value in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn('if(!::CloseHandle(leaf))returnrefuse('
                      '"cannotclosestagingleafinspectionhandleduringorphancleanup");', windows)
        self.assertEqual(windows.count('returntrue;'), 1)
        self.assertIn('AddError(nullptr,std::string(reason)+', missing)
        self.assertIn('#endifreturnrefuse("cannotinspectstagingpayloadleafduringorphancleanup");', posix)
        self.assertIn('if(::lstat(staging.c_str(),&named)==0)returnrefuse(', posix)
        self.assertIn('if(errno==ENOENT)returntrue;', posix)
        for following in ('GetFileAttributesW(', 'GetFileAttributesExW(', 'filesystem::exists(', '::stat('):
            self.assertNotIn(following, missing)
        fixture = compact_cpp((ROOT / "test/persistence/world_save_service_test.cpp")
                              .read_text(encoding="utf-8"))
        self.assertIn('TEST(WorldSaveService,LedgerStagingReclaimsOrphansAfterDirectoryChangesAndReportsOccupiedLeaves)',
                      fixture)

    def test_ledger_staging_is_durable_before_hook_and_replacement(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        writer = service[service.index("boolWorldSaveService::save_active_regions("):]
        writer = writer[:writer.index("std::uint64_tWorldSaveService::region_simulation_digest(")]
        ordered = ("validate_save(save_dir,errors)", "ReclaimAbandonedStaging(staging)",
                   "LedgerStagingtemporary(staging)", "temporary.available(errors)", "temporary.prepare(bytes,errors)",
                   "BeforeActiveRegionsReplaceForTesting()", "temporary.valid(errors)",
                   "temporary.record_payload(*artifacts)",
                   "ReplaceRegionFileAtomically(temporary.path(),path,errors,artifacts)",
                   "if(temporary.installed_at(path))temporary.published(errors)", "ledger=std::move(saved)")
        positions = [writer.index(value) for value in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("if(artifacts&&temporary.installed_at(path,2))temporary.published(errors)", writer)
        transaction = service[service.index("classLedgerStaging{"):service.index("std::size_tReclaimAbandonedStaging(")]
        ordered = ("m_claim.write(claim)", "FlushAncestorEntries(m_path,errors)",
                   "FlushRemovalDirectory(m_path,errors)", "m_payload.promote_seed(m_path,m_occupied,errors)",
                   "m_payload.write(bytes)", "m_record.write(ownership)", "m_ready=true")
        positions = [transaction.index(value) for value in ordered]
        self.assertEqual(positions, sorted(positions))
        fixture = compact_cpp((ROOT / "test/persistence/world_save_service_test.cpp").read_text(encoding="utf-8"))
        for regression in ("LedgerStagingPreservesUnknownOccupantsAndEmptySave",
                           "LedgerStagingReclaimsOnlyValidOwnedPairsIncludingEmptySave",
                           "LedgerStagingOwnershipSurvivesDirectoryAliases",
                           "LedgerStagingPreservesMalformedAndMismatchedOwnership",
                           "LedgerStagingPreservesMissingOrSubstitutedProofAndPayload",
                           "LedgerStagingPreservesPairCopiedFromAnotherWorldOrFilename",
                           "LedgerStagingPreservesSymlinkAndSpecialFileSubstitutions",
                           "LedgerStagingSubstitutionBeforePublishUsesFreshName",
                           "LedgerStagingPreservesUnreadableOwnership",
                           "CreationLedgerStagingCleansFailuresAndJournalsPublication",
                           "LedgerStagingInterruptedWriteRemovesItsPairAndPreservesLiveLedger",
                           "LedgerOwnershipFlushFailurePreservesLiveLedgerAndRemovesOwnPair"):
            self.assertIn("TEST(WorldSaveService," + regression + ")", fixture)
        restart = compact_cpp((ROOT / "test/common/GameSessionHeadlessWorld_test.cpp").read_text(encoding="utf-8"))
        self.assertIn('ASSERT_TRUE(fs::is_regular_file(ownership));', restart)
        self.assertIn('EXPECT_FALSE(fs::exists(ownership));', restart)

    def test_ledger_payload_retains_only_identity_access_through_windows_publication(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        finish = service[service.index("boolfinish_write("):service.index("boolread_record(")]
        ordered = ("identity(before)", "::CloseHandle(m_handle)",
                   "m_handle=INVALID_HANDLE_VALUE", "::CreateFileW(m_path.c_str(),",
                   "access==RetainedAccess::Read?GENERIC_READ:FILE_READ_ATTRIBUTES",
                   "FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE", "identity(after)")
        positions = [finish.index(value) for value in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC", finish)
        self.assertIn("after==before", finish)
        self.assertIn("before[5]==after[5]", finish)
        self.assertIn("std::equal(before.begin(),before.begin()+4,after.begin())", finish)
        record = service[service.index("voidrecord_artifact(CreatedWorldArtifacts&artifacts)"):
                         service.index("boolremove(unsignedlinks=1)")]
        self.assertLess(record.index("CreatedWorldArtifacts::retain_file_identity(m_handle)"),
                        record.index("constboolretired=close()"))
        self.assertIn("m_handle=retained;if(!retired)throwstd::runtime_error(", record)
        self.assertIn("artifacts.record_staged_file(m_path,m_handle)", record)
        self.assertIn("artifacts.record_staged_file(m_path,m_descriptor)", record)
        self.assertNotIn("retain_file_identity", finish)
        prepare = service[service.index("boolprepare(conststd::string&bytes,"):
                          service.index("boolvalid(std::vector<std::string>*errors)")]
        ordered = ("m_payload.write(bytes)",
                   "m_payload.finish_write(StagingFile::RetainedAccess::Identity,errors)",
                   "m_payload.identity(staging_identity)", "constautoownership=StagingOwnershipBytes(",
                   "m_record.write(ownership)",
                   "m_record.finish_write(StagingFile::RetainedAccess::Read,errors)",
                   "m_ready=true")
        positions = [prepare.index(value) for value in ordered]
        self.assertEqual(positions, sorted(positions))
        fixture = compact_cpp((ROOT / "test/persistence/world_save_service_test.cpp")
                              .read_text(encoding="utf-8"))
        self.assertIn("TEST(WorldSaveService,LedgerStagingPublishesCreatesAndReplacementsWithoutDebris)", fixture)

    def test_ledger_claim_is_durable_before_payload_promotion_and_completion(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        transaction = service[service.index("classLedgerStaging{"):
                              service.index("boolStagingNameHasPrefix(")]
        constructor = transaction[:transaction.index("~LedgerStaging()")]
        self.assertIn("m_payload(OwnershipPath(m_path),StagingFile::Mode::Create)", constructor)
        self.assertIn("m_claim(ClaimPath(m_path),StagingFile::Mode::Create)", constructor)
        self.assertNotIn("m_payload(m_path,StagingFile::Mode::Create)", constructor)
        prepare = transaction[transaction.index("boolprepare("):transaction.index("boolvalid(")]
        ordered = ("m_payload.write(seed)", "m_claim.write(claim)",
                   "m_claim.finish_write(StagingFile::RetainedAccess::Read,errors)",
                   "FlushAncestorEntries(m_path,errors)", "FlushRemovalDirectory(m_path,errors)",
                   "m_payload.promote_seed(m_path,m_occupied,errors)",
                   "!FlushRemovalDirectory(m_path,errors))returnfalse;",
                   "m_payload.truncate_payload()", "m_payload.write(bytes)",
                   "m_payload.finish_write(StagingFile::RetainedAccess::Identity,errors)",
                   "m_payload.identity(staging_identity)", "m_record.open(StagingFile::Mode::Create)",
                   "m_record.write(ownership)",
                   "m_record.finish_write(StagingFile::RetainedAccess::Read,errors)",
                   "m_ready=true", "m_claim.remove()", "m_claim.close()")
        positions = []
        offset = 0
        for fragment in ordered:
            offset = prepare.index(fragment, offset)
            positions.append(offset)
        self.assertEqual(positions, sorted(positions))
        promotion = service[service.index("boolpromote_seed("):
                            service.index("booltruncate_payload(")]
        windows, posix = promotion.split("#ifdefined(_WIN32)", 1)[1].split("#else", 1)
        self.assertIn("::MoveFileExW(m_path.c_str(),destination.c_str(),MOVEFILE_WRITE_THROUGH)", windows)
        self.assertNotIn("MOVEFILE_REPLACE_EXISTING", windows)
        self.assertIn("occupied=error==ERROR_FILE_EXISTS||error==ERROR_ALREADY_EXISTS;", windows)
        self.assertEqual(posix.count("occupied=errno==EEXIST;"), 3)
        writer = service[service.index("boolWorldSaveService::save_active_regions("):]
        self.assertIn("if(!temporary.prepare(bytes,errors)){if(temporary.occupied())continue;", writer)
        self.assertIn("SYS_renameat2", posix)
        self.assertIn("RENAME_NOREPLACE", posix)
        self.assertIn("::renamex_np(m_path.c_str(),destination.c_str(),RENAME_EXCL)", posix)
        self.assertLess(posix.index("::link(m_path.c_str(),destination.c_str())"),
                        posix.index("!matches_name(2)||::unlink(m_path.c_str())"))
        writer = service[service.index("boolwrite(conststd::string&bytes)"):
                         service.index("boolpromote_seed(")]
        for flush in ("::FlushFileBuffers(m_handle)", "::fsync(m_descriptor)"):
            self.assertIn(flush, writer)
        self.assertIn("if(!payload_remains&&m_claim_created&&!m_claim.remove())", transaction)

    def test_incomplete_ledger_reclaim_requires_reserved_identity_and_nofollow_inspection(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        reclaim = service[service.index("boolReclaimClaimedLedgerStaging("):
                          service.index("std::filesystem::pathNextLedgerTempPath(")]
        ordered = ("ValidOrphanedStagingOwnership(staging,claim,&reserved)",
                   "reserved!=StablePayloadIdentity(reserved)",
                   "payload.identity(present,false,2)", "StablePayloadIdentity(present)!=reserved",
                   "!payload.matches_name(2)", "seed.identity(seed_identity,false,2)",
                   "StablePayloadIdentity(seed_identity)==reserved", "seed.remove(2)",
                   "seed.close()", "payload.remove()", "payload.close()",
                   "StagingPathMissing(staging)", "claim.remove()")
        positions = [reclaim.index(value) for value in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("ReclaimOrphanedLedgerStaging(staging)", reclaim)
        cleanup = service[service.index("std::size_tReclaimAbandonedStaging("):]
        self.assertIn('if(StagingNameEquals(entry.path().extension(),".claim"))', cleanup)
        self.assertIn("staging.replace_extension();if(ReclaimClaimedLedgerStaging(staging))", cleanup)
        native = service[service.index("classStagingFile{"):
                         service.index("std::filesystem::pathOwnershipPath(")]
        self.assertIn("O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC", native)
        self.assertIn("FILE_FLAG_OPEN_REPARSE_POINT", native)
        self.assertIn("::lstat(m_path.c_str(),&named)", native)
        self.assertIn("info.nNumberOfLinks>links", native)
        self.assertIn("info.st_nlink>links", native)

    def test_claim_interruption_regressions_cover_recovery_and_refusals(self):
        fixture = compact_cpp((ROOT / "test/persistence/world_save_service_test.cpp")
                              .read_text(encoding="utf-8"))
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        # These tests exercise new recovery, rather than only preserving files
        # which the earlier implementation already left untouched.
        self.assertIn("boolReclaimClaimedLedgerStaging(", service)
        for regression in ("LedgerStagingRepeatedClaimInterruptionsDoNotAccumulatePayloads",
                           "LedgerStagingIncompleteClaimsRequireCanonicalProofAndReservedIdentity",
                           "LedgerStagingIncompleteClaimSurvivesTornCompletionAndPayloadWrites",
                           "LedgerStagingIncompleteClaimsRecoverOnlyTheirReservationLinks",
                           "LedgerStagingIncompleteClaimsPreserveNonregularLeavesUntilRestored",
                           "LedgerStagingIncompleteClaimsPreserveUnreadableProofUntilRetry"):
            self.assertIn("TEST(WorldSaveService," + regression + ")", fixture)
        self.assertIn("for(constintpoint:{0,1,2})", fixture)
        self.assertIn("attempt<8", fixture)
        self.assertIn("changed.resize(79)", fixture)
        self.assertIn("changed.assign(513,'x')", fixture)
        self.assertIn("BindOwnershipInstance(interrupted.claim_bytes,interrupted.claim)", fixture)

    def test_staging_refusals_preserve_specific_diagnostics_without_an_errors_vector(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        add_error = service[service.index("voidAddError("):service.index("boolReadAmbientAnchor(")]
        self.assertIn('else{LUMINUMBRA_CORE_ERROR("{}",message);}', add_error)
        for reason in ("cannot identify staging file before closing writer",
                       "cannot close staging writer",
                       "cannot retain unchanged staging identity after closing writer",
                       "cannot write and flush staging payload",
                       "cannot identify staging save directory",
                       "cannot identify completed staging payload",
                       "cannot identify staging ownership instance",
                       "cannot encode staging ownership filename or instance",
                       "cannot write and flush staging ownership",
                       "cannot exclusively create and identify staging payload",
                       "cannot exclusively create and identify staging ownership",
                       "staging ownership has not been prepared",
                       "cannot read complete unchanged staging ownership within 512-byte limit",
                       "staging payload name no longer matches retained identity",
                       "staging ownership name no longer matches retained identity",
                       "staging ownership bytes do not match directory, payload, instance or filename"):
            self.assertIn('refuse("' + re.sub(r"\s+", "", reason) + '"', service)

    def test_windows_case_aliases_cannot_bypass_ledger_ownership_cleanup(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        matching = service[service.index("boolStagingNameHasPrefix("):
                           service.index("std::size_tReclaimAbandonedStaging(")]
        self.assertIn("if(native_name.size()<native_prefix.size())returnfalse;", matching)
        self.assertIn("#ifdefined(_WIN32)", matching)
        self.assertIn("unit>='A'&&unit<='Z'?unit+('a'-'A'):unit", matching)
        self.assertIn("returnfold(left)==fold(right);#elsereturnleft==right;", matching)
        self.assertIn("name.native().size()==expected.native().size()&&StagingNameHasPrefix(name,expected)", matching)
        cleanup = service[service.index("std::size_tReclaimAbandonedStaging("):]
        for call in ('StagingNameHasPrefix(entry.path().filename(),prefix)',
                     'StagingNameEquals(destination.filename(),"active-regions.arl")',
                     'StagingNameEquals(entry.path().extension(),".owner")'):
            self.assertLess(cleanup.index(call), cleanup.index("entry.is_regular_file(kind_error)"))

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
                        save.index("if(m_empty)"))
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
                          "SavedPresetCompatible(save_dir)", "prepared_chunks.prepare(",
                          "prepared_chunks.publish(", "prepared_plants.publish(", "write_file_durably(energy_path,energy_bytes)",
                          "SaveWorldMetadataTo(", "ReconcileWorldStateMetadataTo(",
                          "m_activeRegionLedger.mark_saved(",
                          "chunk->clear_voxel_data_dirty()"):
            self.assertLess(ownership, sequence.index(operation))

    def test_writers_hold_shared_ownership_through_cleanup_and_publication(self):
        service = compact_cpp((ROOT / "src/luminumbra_common/persistence/WorldSaveService.cpp")
                              .read_text(encoding="utf-8"))
        for name in ("save_metadata", "save_metadata_and_active_regions", "save_plant_entities",
                     "save_active_regions", "upsert_container_records", "save_dirty_chunks",
                     "write_file_durably", "remove_file_and_staging",
                     "create_file_durably", "rename_file_durably"):
            start = service.index("WorldSaveService::" + name + "(")
            body = service.index("{", start)
            self.assertTrue(service[body:].startswith("{constSaveGuardguard;"), name)
        plant_publish = service.index("boolWorldSaveService::PreparedPlantWrite::publish(")
        plant_body = service.index("{", plant_publish)
        self.assertTrue(service[plant_body:].startswith("{constSaveGuardguard;"))
        # The prepared records retain ownership from validation through publication.
        data = service[service.index("structWorldSaveService::PreparedWorldWrite::Data{"):
                       service.index("WorldSaveService::PreparedWorldWrite::PreparedWorldWrite()")]
        self.assertIn("SaveOwnershipownership;", data)
        prepare = service[service.index("boolWorldSaveService::PreparedWorldWrite::prepare("):
                          service.index("boolWorldSaveService::PreparedWorldWrite::publish(")]
        self.assertLess(prepare.index("std::make_unique<Data>()"),
                        prepare.index("validate_save(save_dir"))
        self.assertLess(prepare.index("std::make_unique<Data>()"),
                        prepare.index("ReadRegionFile("))
        self.assertIn("m_data=std::move(candidate);", prepare)
        publish = service[service.index("boolWorldSaveService::PreparedWorldWrite::publish("):]
        self.assertLess(publish.index("autoprepared=std::move(m_data);"),
                        publish.index("WriteRegionFile("))
        self.assertLess(publish.index("WriteRegionFile("),
                        publish.index("WriteWorldManifest("))
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
        start = session.index("boolGameSession::SaveWorldStateToImpl(")
        end = session.index("boolGameSession::LoadWorldState()", start)
        energy = session[start:end]
        self.assertIn('WorldSaveService::remove_file_and_staging(energy_path)',
                      energy)
        self.assertIn('WorldSaveService::write_file_durably(energy_path,energy_bytes)',
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
        start = service.index("if(m_empty){")
        end = service.index("returnFlushRemovalDirectory(path,errors);", start)
        empty_roster = service[start:end]
        self.assertIn("std::filesystem::remove(path,ec);}if(ec){", empty_roster)
        self.assertLess(empty_roster.index("LockPublication(path)"),
                        empty_roster.index("std::filesystem::remove(path,ec)"))
        self.assertIn("if(!AllowPublication(path,true,errors)){", empty_roster)
        self.assertIn("returnfalse;", empty_roster)
        self.assertIn("returnFlushRemovalDirectory(path,errors);", service[end:])


if __name__ == "__main__":
    unittest.main()
