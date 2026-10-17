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

    def test_retention_opens_by_id_and_directories_release_the_named_creation_handle(self):
        retention = body(self.journal, "static ScopedFile duplicate_file(")
        self.assertIn("::OpenFileById(", retention)
        self.assertNotRegex(retention, r"::(?:CreateFileW|ReOpenFile|DuplicateHandle)\(")
        self.assertIn("ExtendedFileIdType", retention)
        creation = body(self.journal, "void create_owned_directory(")
        windows = creation.split("#else", 1)[0]
        self.assertRegex(windows, r"recorded\.retained\s*=\s*duplicate_file\(")

    def test_publication_binds_identity_and_detects_same_link_noop_before_claiming_target(self):
        publication = body(self.journal, "void publish_windows(")
        binding = publication.index("const ScopedFile bound(")
        rename = publication.index("::SetFileInformationByHandle(")
        identity = publication.index("identity_from(bound.get())")
        self.assertLess(binding, identity)
        self.assertLess(identity, rename)
        self.assertNotIn("FILE_SHARE_DELETE", publication[binding:identity])
        self.assertIn("const auto published_name = handle_path(bound.get());", publication[rename:])
        self.assertIn("published_name == original_name", publication[rename:])
        self.assertIn("ERROR_ALREADY_EXISTS", publication[rename:])
        caller = body(self.journal, "void publish_file(")
        self.assertLess(caller.index("publish_windows("), caller.index("target.identity ="))

    def test_native_rename_name_comes_from_bound_parent_and_has_checked_storage(self):
        publication = body(self.journal, "void publish_windows(")
        self.assertIn("handle_path(parent.get()) / target.path.filename()", publication)
        self.assertLess(publication.index("destination.make_preferred()"),
                        publication.index("destination.native()"))
        self.assertLess(publication.index("name.size() > 32767"),
                        publication.index("std::vector<std::byte> storage"))
        parent = body(self.journal, "ScopedFile bind_publication_parent(")
        self.assertIn("identity_from(parent.get()) != find_artifact(path).identity", parent)
        self.assertNotIn("FILE_SHARE_DELETE", parent)
        for artifact in ("source", "target"):
            self.assertIn(f"bind_publication_parent({artifact}.path.parent_path())", publication)

    def test_native_rename_record_includes_an_explicit_uncounted_terminator(self):
        publication = body(self.journal, "void publish_windows(")
        compact = re.sub(r"\s+", " ", publication)
        self.assertIn("offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t)", compact)
        self.assertIn("rename_info->FileNameLength = static_cast<DWORD>(name_bytes);", compact)
        terminator = "rename_info->FileName[name.size()] = L'\\0';"
        self.assertIn(terminator, publication)
        self.assertLess(publication.index(terminator),
                        publication.index("::SetFileInformationByHandle("))

    def test_publication_requires_the_exact_destination_before_claiming_ownership(self):
        publication = body(self.journal, "void publish_windows(")
        rename = publication.index("::SetFileInformationByHandle(")
        self.assertIn("published_name != destination", publication[rename:])
        self.assertIn("ERROR_INVALID_DATA", publication[rename:])
        native = (ROOT / "test/persistence/created_world_artifacts_test.cpp").read_text(
            encoding="utf-8")
        self.assertIn("UnexpectedPublicationNameNeverClaimsTheDestination", native)
        self.assertIn("PublicationPreservesExactLeafNamesAcrossRenameBufferLengths", native)
        self.assertIn("EXPECT_LT(length_in_chars, capacity);", native)
        self.assertIn("EXPECT_EQ(rename_info->FileName[length_in_chars], L'\\0');", native)

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

    def test_native_regressions_and_legacy_fault_discrimination_remain_registered(self):
        native = (ROOT / "test/persistence/created_world_artifacts_test.cpp").read_text(
            encoding="utf-8")
        for api in ("MoveFileExW", "ReOpenFile", "DuplicateHandle", "OpenFileById",
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
