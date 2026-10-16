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
        self.assertIn("constautouser_settings=g_systemConfig.user();"
                      "g_systemConfig=*scenario_config.systems_config_override;"
                      "g_systemConfig.user()=user_settings;", self.client)

    def test_server_boot_and_admission_share_root_loader(self):
        for source, root in ((self.runner, "m_config.root_path"), (self.admission, "root")):
            self.assertIn("luminumbra::core::SystemConfig::LoadFromRuntimeRoot(" + root + ")",
                          source)
            self.assertNotIn('"data/common/systems.json"', source)

    def test_ui_watchers_use_runtime_root(self):
        for extension in ("rml", "rcss"):
            self.assertIn('uiHotReload.WatchDirectory(root_path_str+"data/ui","' +
                          extension + '");', self.client)
            self.assertNotIn('uiHotReload.WatchDirectory("data/ui","' + extension + '");',
                             self.client)


if __name__ == "__main__":
    unittest.main()
