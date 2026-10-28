"""Static ownership guards; native filesystem qualification runs separately."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
PERSISTENCE = ROOT / "src/luminumbra_common/persistence"


def body(text, marker):
    masked = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()), text, flags=re.S,
    )
    start = masked.index("{", text.index(marker))
    depth = 1
    for index in range(start + 1, len(masked)):
        depth += (masked[index] == "{") - (masked[index] == "}")
        if depth == 0:
            return text[start + 1:index]
    raise AssertionError(f"Unclosed function: {marker}")


class WorldEntryFilesystemWiring(unittest.TestCase):
    def setUp(self):
        self.journal = (PERSISTENCE / "CreatedWorldArtifacts.h").read_text(encoding="utf-8")

    def test_retention_uses_an_explicit_volume_and_releases_the_named_creation_handle(self):
        retention = body(self.journal, "static ScopedFile duplicate_file(")
        self.assertIn("::OpenFileById(volume.get(),", retention)
        self.assertNotRegex(retention, r"::(?:ReOpenFile|DuplicateHandle)\(")
        self.assertIn("VOLUME_NAME_GUID | FILE_NAME_NORMALIZED", retention)
        self.assertRegex(retention, r"const ScopedFile volume\(\s*::CreateFileW\(volume_path.c_str\(\),")
        self.assertIn("volume_path.resize(separator)", retention)
        self.assertIn("volume.get() == INVALID_HANDLE_VALUE", retention)
        self.assertIn("length == 0 || length >= volume_name.size()", retention)
        self.assertIn("separator == std::wstring::npos", retention)
        self.assertIn("Unknown world artifact retention identity.", retention)
        self.assertIn("ExtendedFileIdType", retention)
        creation = body(self.journal, "void create_owned_directory(")
        windows = creation.split("#else", 1)[0]
        self.assertRegex(windows, r"recorded\.retained\s*=\s*duplicate_file\(")

    def test_publication_binds_source_identity_before_exclusive_link_creation(self):
        publication = body(self.journal, "void publish_windows(")
        binding = publication.index("ScopedFile bound(")
        link = publication.index("Detail::WindowsPublishWithoutReplacement(")
        identity = publication.index("identity_from(bound.get())")
        self.assertLess(binding, identity)
        self.assertLess(identity, link)
        self.assertNotIn("FILE_SHARE_DELETE", publication[binding:identity])
        self.assertIn("bound.release(), source.path, destination", publication[link:])
        caller = body(self.journal, "void publish_file(")
        self.assertLess(caller.index("publish_windows("), caller.index("target.identity ="))

    def test_publication_checks_source_retirement_and_retains_verified_cleanup_ownership(self):
        publication = body(self.journal, "inline HANDLE WindowsPublishWithoutReplacement(")
        link = publication.index("::CreateHardLinkW(")
        retire = publication.index("original.get(), FileDispositionInfo")
        self.assertLess(link, retire)
        self.assertLess(retire, publication.index("original.reset();"))
        self.assertLess(publication.index("WindowsVacantPublicationNameError(original_name)"),
                        publication.index("return linked.release();"))
        self.assertIn("retirement_error != ERROR_SUCCESS", publication)
        journal = body(self.journal, "void publish_windows(")
        recovery = journal[journal.index("catch (const Detail::WindowsPublicationRecoveryError&") :]
        self.assertIn("if (error.verified_link())", recovery)
        self.assertIn("target.identity = source.identity", recovery)
        self.assertIn("throw;", recovery)

    def test_every_windows_exclusive_publication_uses_atomic_link_creation_and_exact_counts(self):
        publication = body(self.journal, "inline HANDLE WindowsPublishWithoutReplacement(")
        vacant = publication.index("WindowsVacantPublicationNameError(bound_destination)")
        before = publication.index("WindowsPublicationLinkCount(original.get()) != 1")
        link = publication.index("::CreateHardLinkW(")
        two = publication.index("WindowsPublicationLinkCount(original.get()) != 2")
        verified = publication.index("verified_link = true")
        retire = publication.index("original.get(), FileDispositionInfo")
        one = publication.index("WindowsPublicationLinkCount(linked.get()) != 1")
        self.assertLess(vacant, link)
        self.assertLess(before, link)
        self.assertLess(link, two)
        self.assertLess(two, verified)
        self.assertLess(verified, retire)
        self.assertLess(retire, one)
        self.assertLess(one, publication.index("return linked.release();"))
        self.assertIn("vacancy != ERROR_SUCCESS", publication[vacant:link])
        self.assertIn("supported same-volume hard links", publication[link:two])
        self.assertNotRegex(publication, r"::(?:MoveFileExW|ReplaceFileW)\(|FileRenameInfo")
        self.assertIn("retained_info.nFileIndexLow != published_info.nFileIndexLow", publication)
        check = body(self.journal, "inline DWORD WindowsVacantPublicationNameError(")
        for token in ("::CreateFileW(destination.c_str()", "FILE_READ_ATTRIBUTES",
                      "FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE",
                      "FILE_FLAG_OPEN_REPARSE_POINT", "occupant != INVALID_HANDLE_VALUE",
                      "return ERROR_ALREADY_EXISTS", "error == ERROR_FILE_NOT_FOUND"):
            self.assertIn(token, check)
        count = body(self.journal, "inline DWORD WindowsPublicationLinkCount(")
        self.assertIn("FileStandardInfo", count)
        self.assertIn("info.NumberOfLinks == 0", count)
        recovery = body(self.journal, "inline void WindowsRestorePublicationSource(")
        self.assertIn("link->ReplaceIfExists = FALSE", recovery)
        self.assertIn('"NtSetInformationFile"', recovery)
        self.assertNotRegex(recovery, r"::(?:DeleteFileW|MoveFileExW)\(")
        writer = (PERSISTENCE / "WorldSaveService.cpp").read_text(encoding="utf-8")
        self.assertEqual(writer.count("Detail::WindowsPublishWithoutReplacement("), 2)
        self.assertNotIn("::MoveFileExW(", writer)
        for marker in ("bool ReplaceRegionFileAtomically(", "bool Publish("):
            publishing = body(writer, marker).split("#else", 1)[0]
            linking = publishing.index("Detail::WindowsPublishWithoutReplacement(")
            flush = publishing.index("::FlushFileBuffers(", linking)
            self.assertLess(linking, flush)
            binding = publishing.index("::CreateFileW(")
            self.assertNotIn("FILE_SHARE_DELETE", publishing[binding:linking])

    def test_windows_link_collisions_cover_preexisting_and_late_aliases(self):
        native = (ROOT / "test/persistence/created_world_artifacts_test.cpp").read_text(
            encoding="utf-8")
        opening = body(native[native.rindex("HANDLE WINAPI Open("):], "HANDLE WINAPI Open(")
        binding = opening.index("(access & DELETE) != 0")
        collision = opening.index("active->boundary == Boundary::PublicationLinkedOccupant",
                                  binding)
        self.assertLess(collision, opening.index("BeforePublish(path, active->target)", binding))
        self.assertLess(collision, opening.index("return ::CreateFileW("))
        hook = body(native, "bool BeforePublish(")
        self.assertIn("std::filesystem::create_hard_link(source, destination)", hook)
        self.assertIn("EXPECT_TRUE(std::filesystem::equivalent(source, destination))", hook)
        regression = body(native, "TEST_F(CreatedWorldCleanup, "
                          "RefusedPublicationNeverClaimsARacingHardLinkToTheSource)")
        self.assertLess(regression.index("std::filesystem::equivalent(temp, destination)"),
                        regression.index("artifacts.rollback()"))
        late = body(native, "TEST_F(CreatedWorldCleanup, "
                    "LinkCreationAfterTheVacancyCheckIsNeverOverwrittenOrClaimed)")
        for token in ("ERROR_ALREADY_EXISTS", "EXPECT_FALSE(Race().publication_completed)",
                      "std::filesystem::equivalent(temp, destination)",
                      'EXPECT_EQ(Read(temp), "owned bytes\\n")', "artifacts.rollback()"):
            self.assertIn(token, late)
        self.assertIn("#define CreateHardLinkW NativeRace::Link", native)
        link = body(native[native.rindex("BOOL WINAPI Link("):], "BOOL WINAPI Link(")
        self.assertLess(link.index("BeforePublish("), link.index("::CreateHardLinkW("))

    def test_native_link_names_come_from_bound_parents_and_have_checked_lengths(self):
        publication = body(self.journal, "void publish_windows(")
        self.assertIn("handle_path(parent.get()) / target.path.filename()", publication)
        self.assertLess(publication.index("destination.make_preferred()"),
                        publication.index("Detail::WindowsPublishWithoutReplacement("))
        linking = body(self.journal, "inline HANDLE WindowsPublishWithoutReplacement(")
        self.assertLess(linking.index("bound_destination.native().size() > 32767"),
                        linking.index("::CreateHardLinkW("))
        self.assertIn("original_name = WindowsPublicationPath(original.get())", linking)
        parent = body(self.journal, "ScopedFile bind_publication_parent(")
        self.assertIn("identity_from(parent.get()) != find_artifact(path).identity", parent)
        self.assertNotIn("FILE_SHARE_DELETE", parent)
        for artifact in ("source", "target"):
            self.assertIn(f"bind_publication_parent({artifact}.path.parent_path())", publication)

    def test_native_link_recovery_record_has_bounded_counted_storage(self):
        recovery = body(self.journal, "inline void WindowsRestorePublicationSource(")
        compact = re.sub(r"\s+", " ", recovery)
        self.assertIn("offsetof(LinkInformation, FileName) + name_bytes", compact)
        self.assertIn("link->FileNameLength = static_cast<ULONG>(name_bytes);", compact)
        self.assertLess(recovery.index("win32_name.size() > 32767"),
                        recovery.index("std::vector<std::byte> storage"))
        self.assertIn("std::memcpy(link->FileName, name.data(), name_bytes)", recovery)
        # CreateHardLinkW consumes the paths' own null-terminated wide strings;
        # publication no longer constructs a variable-sized rename record.
        linking = body(self.journal, "inline HANDLE WindowsPublishWithoutReplacement(")
        self.assertIn("::CreateHardLinkW(bound_destination.c_str(), original_name.c_str(), nullptr)",
                      linking)
        self.assertNotIn("FILE_RENAME_INFO", linking)

    def test_publication_requires_the_exact_destination_and_rolls_back_through_its_handle(self):
        publication = body(self.journal, "inline HANDLE WindowsPublishWithoutReplacement(")
        self.assertIn("WindowsPublicationPath(linked.get()) != bound_destination", publication)
        self.assertIn("ERROR_INVALID_DATA", publication)
        recovery = publication[publication.index("catch (const std::exception& error)"):]
        self.assertIn("WindowsRestorePublicationSource(linked.get(), original_name)", recovery)
        self.assertIn("linked.get(), FileDispositionInfo", recovery)
        self.assertNotRegex(recovery, r"::(?:DeleteFileW|MoveFileExW)\(")
        native = (ROOT / "test/persistence/created_world_artifacts_test.cpp").read_text(
            encoding="utf-8")
        self.assertIn("RefusedPublicationInspectionRestoresSourceAndRetiresOnlyTheCreatedLink", native)
        for fault in ("PublicationFinalNameMismatch", "PublicationSourceInspectionFailure",
                      "PublicationDestinationIdentityUnknown", "PublicationSourceUnlinkDenied"):
            self.assertIn(fault, native)
        self.assertIn("PublicationPreservesExactLeafNamesAcrossHardLinkPathLengths", native)
        self.assertIn("InterruptedLinkedPublicationRetainsBothNamesUntilRollbackCanRetry", native)

    def test_posix_publication_verifies_the_quarantined_entry_before_descriptor_relative_move(self):
        self.assertIn("publish_posix(source, target);", body(self.journal, "void publish_file("))
        publication = body(self.journal, "void publish_posix(")
        self.assertLess(publication.index("const Quarantine quarantine("),
                        publication.index("::fstatat("))
        self.assertLess(publication.index("moved != source.identity"),
                        publication.index('container.get(), "artifact", target_parent.get()'))
        self.assertIn('source_parent.get(), source_name.c_str(), container.get(), "artifact"',
                      re.sub(r"\s+", " ", publication))
        self.assertIn("restore_quarantined(", publication)
        self.assertNotIn("AT_FDCWD", publication)

    def test_journal_and_durable_writer_never_convert_native_paths_through_the_code_page(self):
        writer = (PERSISTENCE / "WorldSaveService.cpp").read_text(encoding="utf-8")
        session = (ROOT / "src/luminumbra_common/world/GameSession.cpp").read_text(encoding="utf-8")
        for source in (self.journal, writer, session):
            self.assertNotRegex(source, r"\.(?:string|generic_string)\(")
            self.assertIn("Filesystem::GenericUtf8PathString(", source)
        self.assertIn('const fs::path worldPath = GetSaveRootPath() / "worlds/saves" / worldId;',
                      session)
        reclaim = body(writer, "std::size_t ReclaimAbandonedStaging(")
        self.assertIn("destination.filename().native()", reclaim)
        self.assertIn("entry.path().filename().native()", reclaim)

    def test_journal_metadata_keeps_calendar_normalization_and_serialized_writers(self):
        writer = (PERSISTENCE / "WorldSaveService.cpp").read_text(encoding="utf-8")
        metadata = body(writer, "bool WorldSaveService::save_metadata(")
        self.assertIn("const SaveGuard guard;", metadata)
        self.assertIn("clock.write_metadata(metadata);", metadata)
        self.assertIn('metadata["renderCalendar"] = world::WorldClock(0, *calendar).calendar_metadata();',
                      metadata)
        self.assertRegex(metadata, r"WriteDurableRegionTemp\(destination, output_bytes, temp, errors, artifacts\)")
        self.assertLess(metadata.index("read_clock_metadata("),
                        metadata.index("artifacts->reserve_file(destination)"))
        paired = body(writer, "bool WorldSaveService::save_metadata_and_active_regions(")
        self.assertIn("const SaveGuard guard;", paired)
        for call in ("save_metadata(bytes, save_dir, errors, artifacts)",
                     "save_active_regions(ledger, clock, save_dir, errors, artifacts)"):
            self.assertEqual(paired.count(call), 2)

    def test_snapshot_refusals_keep_the_live_world_or_change_revision_when_it_is_torn_down(self):
        session = (ROOT / "src/luminumbra_common/world/GameSession.cpp").read_text(encoding="utf-8")
        for method in ("LoadWorld", "LoadWorldFrom"):
            with self.subTest(method=method):
                loading = body(session, f"bool GameSession::{method}(")
                match = re.search(r"if \(!ReadWorldStateFrom\([^;]*,\s*loaded,\s*true\)\)\s*return false;", loading)
                self.assertIsNotNone(match)
                # A refused candidate never touches the live world, so the revision stays put.
                self.assertLess(match.start(), loading.index("ResetWorldSystems();"))
        with self.subTest(method="LoadWorldStateFrom"):
            loading = body(session, "bool GameSession::LoadWorldStateFrom(")
            match = re.search(r"if \(!ReadWorldStateFrom\([^;]*,\s*state\)\) \{", loading)
            self.assertIsNotNone(match)
            refusal = loading[match.end():].split("return false;", 1)[0]
            refusal = body(refusal, "if (!state.input_refused)")
            self.assertIn("++m_worldRevision;", refusal)
            self.assertLess(refusal.index("++m_worldRevision;"),
                            refusal.index("m_worldSystem->clear_world("))
            self.assertIn("m_worldUsable = false;", refusal)

    def test_async_persistence_keeps_refusal_latches_and_checks_optional_in_adoption(self):
        machine = (ROOT / "src/luminumbra_client/world/WorldEntry.h").read_text(encoding="utf-8")
        advancing = body(machine, "void advance(")
        self.assertLess(advancing.index("!m_request.has_value()"),
                        advancing.index("run(m_stage, *m_request)"))
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        predicate = body(source, "const auto session_persistence_allowed =")
        self.assertIn("entry_save_allowed &&", predicate)
        self.assertIn("SessionPersistenceAllowed(", predicate)
        drain = body(source, "auto drain_entry =")
        compact = re.sub(r"\s+", " ", drain)
        self.assertIn("entry_save_allowed = entry_previous_save_allowed && "
                      "gameSession->WorldRevision() == entry_world_revision;", compact)
        self.assertLess(compact.index("jobSystem.wait(entry_cpu_job)"),
                        compact.index("entry_save_allowed = entry_previous_save_allowed"))
        requested = source.split("if (world_entry.state() == WorldEntryState::Requested) {", 1)[1]
        self.assertLess(requested.index("entry_previous_save_allowed = entry_save_allowed;"),
                        requested.index("entry_save_allowed = false;"))
        adoption = source.split("case WorldEntryStage::AdoptWaterfalls:", 1)[1].split(
            "case WorldEntryStage::PrepareChunks:", 1)[0]
        self.assertLess(adoption.index("if (!entry_geometry.has_value())"),
                        adoption.index("entry_geometry.value()"))
        self.assertRegex(adoption, r"if \(!entry_geometry.has_value\(\)\) \{[^}]*"
                         r"return WorldEntryResult::Failed;")
        for marker, phase in (("g_uiManager->SetWorldCreationCallback(", "WorldCreateCallback"),
                              ("g_uiManager->SetLoadWorldCallback(", "WorldLoadCallback")):
            callback = body(source, marker)
            self.assertIn("Attribution::Phase::" + phase, callback)
            self.assertIn("world_entry.request(", callback)

    def test_async_snapshot_verification_precedes_reader_drain_and_retains_authority(self):
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        stages = body(source, "auto run_entry_stage =")
        validation = stages.split("case WorldEntryStage::Validate:", 1)[1].split(
            "case WorldEntryStage::DrainReaders:", 1)[0]
        self.assertLess(validation.index("if (!request.create)"),
                        validation.index("Measurement::require_workload_world_load("))
        self.assertLess(validation.index("Measurement::require_workload_world_load("),
                        validation.index("jobSystem.dispatch_batch("))
        self.assertRegex(validation, r"catch \(const std::exception& error\) \{\s*"
                         r"refuse_world_entry\(error.what\(\), false\);\s*"
                         r"return WorldEntryResult::Failed;")
        self.assertIn("save_directory = gameSession->GetWorldSaveDir()", validation)
        self.assertIn("saved_digest = benchmark_saved_world_digest", validation)
        self.assertIn("entry_worker_error.empty() && benchmark_v4 &&", validation)
        self.assertIn("!entry_request->create", validation)
        self.assertRegex(validation, r"std::make_unique<Measurement::VerifiedSaveSnapshot>\(\s*"
                         r"save_directory, saved_digest, entry_request->id\)")
        self.assertLess(validation.index("jobSystem.wait(entry_cpu_job)"),
                        validation.index("if (!error.empty())"))
        self.assertIn('std::string("Could not schedule world validation.")', validation)
        opening = stages.split("case WorldEntryStage::OpenWorld:", 1)[1].split(
            "case WorldEntryStage::RestoreState:", 1)[0]
        self.assertIn("gameSession->LoadWorldFrom(request.id, entry_load_snapshot->root())", opening)
        self.assertIn("gameSession->LoadWorld(request.id)", opening)
        self.assertLess(opening.index("return WorldEntryResult::Failed;"),
                        opening.index("benchmark_loaded_snapshot = std::move(entry_load_snapshot)"))
        drain = body(source, "auto drain_entry =")
        self.assertLess(drain.index("jobSystem.wait(entry_cpu_job)"),
                        drain.index("entry_load_snapshot.reset()"))
        self.assertLess(drain.index("prepare_world_entry()"),
                        drain.index("entry_load_snapshot.reset()"))

    def test_async_creation_restores_roundtrip_authority_before_tracking_capture_identity(self):
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        stages = body(source, "auto run_entry_stage =")
        restoration = stages.split("case WorldEntryStage::RestoreState:", 1)[1].split(
            "case WorldEntryStage::PrepareWaterfalls:", 1)[0]
        self.assertIn("!request.create ||", restoration)
        self.assertIn("scenario_config.persistence_roundtrip_smoke() &&", restoration)
        self.assertIn('scenario_config.persistence_phase == "load" &&', restoration)
        self.assertIn("!scenario_config.persistence_session_dir.empty()", restoration)
        self.assertIn("gameSession->LoadWorldStateFrom(scenario_config.persistence_session_dir)",
                      restoration)
        self.assertIn("gameSession->LoadWorldState()", restoration)
        self.assertRegex(restoration, r"if \(!restored\) \{[^}]*return WorldEntryResult::Failed;")
        activation = stages.split("case WorldEntryStage::Activate:", 1)[1]
        self.assertRegex(activation, r"if \(benchmark_v4 && request.create\)\s*"
                         r"benchmark_created_world_id = gameSession->GetMetadata\(\).worldId;")
        self.assertNotIn("benchmark_created_world_id =", restoration)

    def test_staged_capture_entry_waits_for_activation_and_uses_configured_surface_budget(self):
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        preparation = body(source, "auto finish_world_entry =")
        self.assertRegex(preparation, r"const bool bypass_loading_ui =\s*!staged_entry &&")
        horizon = body(source, "auto prepare_entry_horizon =")
        self.assertIn("capture_horizon ? scenario_config.horizon_radius : 12", horizon)
        self.assertIn("capture_horizon ? scenario_config.collision_radius : 4", horizon)
        for condition in ("benchmark_v4 ||", "runtime_boot_recorder.enabled() ||",
                          "scenario_config.active() && scenario_config.auto_enter_world"):
            self.assertIn(condition, horizon)
        activation = body(source, "auto activate_loaded_world =")
        self.assertIn("(!scenario_config.active() || benchmark_v4) && g_uiManager", activation)
        self.assertIn("scenario_config.active() || benchmark_v4 || runtime_boot_recorder.enabled()",
                      activation)
        stages = body(source, "auto run_entry_stage =")
        refusal = stages.split("case WorldEntryStage::PrepareHorizon:", 1)[1].split(
            "case WorldEntryStage::Activate:", 1)[0]
        self.assertRegex(refusal, r"if \(!prepare_entry_horizon\(\)\) \{[^}]*"
                         r"return WorldEntryResult::Failed;")

    def test_far_cache_drains_before_saves_cleanup_and_live_preset_changes(self):
        source = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        pause = source.split('} else if (act == "quit") {', 1)[1].split(
            'SetGameState(window, gameStateManager, GameState::MAIN_MENU)', 1)[0]
        self.assertLess(pause.index("if (!session_persistence_allowed())"),
                        pause.index("prepare_world_entry();"))
        self.assertLess(pause.index("prepare_world_entry();"), pause.index("SaveWorld()"))
        benchmark = source.split("[&](bool main_menu) {", 1)[1].split("[&](bool reload)", 1)[0]
        self.assertLess(benchmark.index("prepare_world_entry();"), benchmark.index("SaveWorld()"))
        boot = source.split("const bool wrote_metrics = runtime_boot_recorder.write_artifacts();", 1)[1]
        self.assertLess(boot.index("prepare_world_entry();"), boot.index("clear_world(nullptr)"))
        viewer = (ROOT / "src/luminumbra_client/debug/WorldGenViewer.cpp").read_text(encoding="utf-8")
        application = viewer.split('if (ImGui::Button("Apply & Regenerate"))', 1)[1]
        self.assertLess(application.index("pipeline.prepare_world_swap();"),
                        application.index("main_world_system->set_params(m_params)"))
        self.assertIn("worldGenViewer->DrawContents(liveWorld, renderPipeline);", source)
        scenario = (ROOT / "src/luminumbra_client/core/ScenarioRunnerDrive.cpp").read_text(encoding="utf-8")
        saving = body(scenario, "bool ScenarioRunnerImpl::drivePersistenceRoundtrip()")
        self.assertLess(saving.index("renderPipeline.prepare_world_swap();"),
                        saving.index("RunPersistenceRoundtripSavePhase("))

    def test_far_snapshot_checks_publication_before_reading_live_vectors(self):
        source = (ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp").read_text(
            encoding="utf-8")
        capture = body(source, "SHIELD_WorldSystem::capture_far_lod_sdf_snapshot(")
        self.assertRegex(capture, r"if \(!chunk \|\| chunk->get_state\(\) == ChunkState::Loading \|\|")
        self.assertLess(capture.index("chunk->get_state() == ChunkState::Loading"),
                        capture.index("chunk->sdf_data.size()"))
        self.assertLess(capture.index("chunk->get_state() == ChunkState::Loading"),
                        capture.index("chunk->material_data.size()"))
        self.assertLess(capture.index("chunk->get_state() == ChunkState::Loading"),
                        capture.index("chunk->capture_far_lod_sdf("))
        native = (ROOT / "test/rendering/far_lod_persistence_test.cpp").read_text(encoding="utf-8")
        self.assertIn("CapturesWaitForGenerationPublication", native)

    def test_native_regressions_and_legacy_fault_discrimination_remain_registered(self):
        native = (ROOT / "test/persistence/created_world_artifacts_test.cpp").read_text(
            encoding="utf-8")
        for api in ("MoveFileExW", "CreateHardLinkW", "ReOpenFile", "DuplicateHandle", "OpenFileById",
                    "SetFileInformationByHandle"):
            self.assertIn(f"#define {api} NativeRace::", native)
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("    created_world_artifacts_test\n", cmake)
        self.assertIn("    world_entry_test\n", cmake)
        target = cmake.split("add_executable(created_world_artifacts_test", 1)[1].split(")", 1)[0]
        self.assertIn("../src/luminumbra_common/core/FilesystemPath.cpp", target)
        own_path = "test/persistence/test_world_entry_filesystem_wiring.py"
        self.assertIn(own_path, cmake)
        for registration in ("tools/ci/ci_paths.py", ".github/workflows/ci.yml"):
            self.assertIn(own_path, (ROOT / registration).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
