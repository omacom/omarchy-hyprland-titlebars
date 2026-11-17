"""Check the desktop-entry opt-in without starting a compositor."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class DesktopEntryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='titlebars-entry-')
        root = Path(__file__).resolve().parents[1]
        driver = Path(cls.build.name) / 'driver.cpp'
        driver.write_text('''#include "appName.hpp"
#include <iostream>
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::cout << omarchyAppName(argv[1]) << "\\n" << omarchyAppHasClientControls(argv[1]) << "\\n";
}
''')
        cls.probe = Path(cls.build.name) / 'probe'
        subprocess.run(['c++', '-std=c++23', '-I', str(root), str(driver), str(root / 'appName.cpp'), '-o', str(cls.probe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='titlebars-entry-data-')
        self.addCleanup(self.scratch.cleanup)
        root = Path(self.scratch.name)
        self.user = root / 'user/applications'
        self.system = root / 'system/applications'
        self.user.mkdir(parents=True)
        self.system.mkdir(parents=True)
        self.env = dict(os.environ, XDG_DATA_HOME=str(self.user.parent), XDG_DATA_DIRS=str(self.system.parent))

    def entry(self, directory, name, body):
        (directory / name).write_text('[Desktop Entry]\nName=Example\n' + body)

    def resolve(self, window_class):
        lines = subprocess.check_output([str(self.probe), window_class], env=self.env, text=True).splitlines()
        return lines[0], lines[1] == '1'

    def test_explicit_class_and_file_id_use_same_entry_for_both_properties(self):
        self.entry(self.system, 'example.desktop', 'StartupWMClass=ExampleClass\nX-Omarchy-ClientControls=true\n')
        self.assertEqual(self.resolve('ExampleClass'), ('Example', True))
        self.assertEqual(self.resolve('EXAMPLE'), ('Example', True))

    def test_user_entry_can_disable_system_opt_in(self):
        self.entry(self.system, 'example.desktop', 'X-Omarchy-ClientControls=true\n')
        self.entry(self.user, 'example.desktop', 'X-Omarchy-ClientControls=false\n')
        self.assertEqual(self.resolve('example'), ('Example', False))

    def test_absent_or_non_boolean_declaration_does_not_enable_controls(self):
        for value in ['', 'X-Omarchy-ClientControls=TRUE\n', 'X-Omarchy-ClientControls=exec something\n']:
            self.entry(self.user, 'example.desktop', value)
            self.assertEqual(self.resolve('example'), ('Example', False))

    def test_action_group_cannot_declare_window_controls(self):
        self.entry(self.user, 'example.desktop', '[Desktop Action New]\nX-Omarchy-ClientControls=true\n')
        self.assertEqual(self.resolve('example'), ('Example', False))

    def test_unknown_and_empty_classes_have_no_implicit_opt_in(self):
        self.entry(self.user, 'example.desktop', 'X-Omarchy-ClientControls=true\n')
        self.assertEqual(self.resolve('missing'), ('', False))
        self.assertEqual(self.resolve(''), ('', False))


if __name__ == '__main__':
    unittest.main()
