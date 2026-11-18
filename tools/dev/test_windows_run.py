"""Regression checks for non-interactive development command formation."""
import unittest

from windows_run import _unattended_ctest


class CTestArguments(unittest.TestCase):
    def test_direct_invocation(self):
        command = [r"C:\Build Tools\bin\CTest.exe", "--output-on-failure"]
        _unattended_ctest(command)
        self.assertEqual(command[-2:], ["--interactive-debug-mode", "0"])
        again = command.copy()
        _unattended_ctest(again)
        self.assertEqual(again, command)

    def test_forwarded_environment(self):
        command = [r"C:\Tools\cmake.exe", "-E", "env", "--unset=KEY",
                   "--modify", "PATH=path_list_prepend:C:/Tools", "EXTRA=1", "--",
                   r"C:\Tools\ctest.exe", "-C", "Release"]
        original = command.copy()
        _unattended_ctest(command)
        self.assertEqual(command, original + ["--interactive-debug-mode", "0"])

    def test_interactive_option_is_overridden_before_forwarded_arguments(self):
        command = [r"C:\Tools\ctest.exe", "--interactive-debug-mode", "1", "--",
                   "--interactive-debug-mode", "1"]
        _unattended_ctest(command)
        self.assertEqual(command[1:], ["--interactive-debug-mode", "1",
                                       "--interactive-debug-mode", "0", "--",
                                       "--interactive-debug-mode", "1"])

    def test_nested_cmake_wrappers(self):
        command = [r"C:\Tools\cmake.exe", "-E", "env", "EXTRA=1", "--",
                   "cmake", "-E", "time", "cmake.exe", "-E", "chdir", r"E:\Build Files",
                   "ctest", "-R", "^bounded_native_"]
        original = command.copy()
        _unattended_ctest(command)
        self.assertEqual(command, original + ["--interactive-debug-mode", "0"])

    def test_build_tool_arguments_remain_unchanged(self):
        command = [r"C:\Tools\ctest.exe", "--build-and-test", "source", "build",
                   "--build-options", "-Dfoo=bar", "--test-command", "tool", "-V"]
        _unattended_ctest(command)
        self.assertEqual(command[4:], ["--interactive-debug-mode", "0",
                                      "--build-options", "-Dfoo=bar", "--test-command", "tool", "-V"])

    def test_other_commands_are_never_rewritten(self):
        for command in (
            [r"C:\Tools\other.exe", r"C:\Tools\ctest.exe"],
            [r"C:\Tools\cmake.exe", "-E", "env", "--", "other.exe", "ctest.exe"],
            [r"C:\Tools\cmake.exe", "-E", "env", "--modify"],
            [r"C:\Tools\cmake.exe", "-E", "chdir"],
            [r"C:\Tools\cmake.exe", "-E", "time"],
        ):
            with self.subTest(command=command):
                original = command.copy()
                _unattended_ctest(command)
                self.assertEqual(command, original)


if __name__ == "__main__":
    unittest.main()
