"""CPU-only structural regression fixtures for runtime export admission."""
import struct
import unittest

from hip_runtime_exports import pe_function_exports

PE, OPTIONAL, SECTIONS, DIRECTORY = 0x80, 0x98, 0x188, 0x400


def put(data, offset, fmt, value):
    struct.pack_into(fmt, data, offset, value)


def fixture():
    data = bytearray(0xe00)
    data[:2] = b"MZ"
    put(data, 0x3c, "<I", PE)
    data[PE:PE+4] = b"PE\0\0"
    put(data, PE+4, "<H", 0x8664)
    put(data, PE+6, "<H", 2)
    put(data, PE+20, "<H", 240)
    put(data, OPTIONAL, "<H", 0x20b)
    put(data, OPTIONAL+108, "<I", 16)
    put(data, OPTIONAL+112, "<I", 0x1000)
    put(data, OPTIONAL+116, "<I", 0x600)
    for offset, virtual, raw_size, raw_offset, flags in (
            (SECTIONS, 0x1000, 0x800, 0x400, 0x40000040),
            (SECTIONS+40, 0x2000, 0x200, 0xc00, 0x60000020)):
        put(data, offset+12, "<I", virtual)
        put(data, offset+16, "<I", raw_size)
        put(data, offset+20, "<I", raw_offset)
        put(data, offset+36, "<I", flags)
    for offset, value in ((20, 2), (24, 2), (28, 0x1100), (32, 0x1200), (36, 0x1300)):
        put(data, DIRECTORY+offset, "<I", value)
    for offset, value in ((0x500, 0x2000), (0x504, 0x2001), (0x600, 0x1400), (0x604, 0x1440)):
        put(data, offset, "<I", value)
    put(data, 0x700, "<H", 0)
    put(data, 0x702, "<H", 1)
    for offset, name in ((0x800, b"hipInit"), (0x840, b"hipGetDevicePropertiesR0600")):
        data[offset:offset+len(name)] = name
    return data


class ExportAdmission(unittest.TestCase):
    def test_direct_functions(self):
        self.assertEqual(pe_function_exports(fixture()), {"hipInit", "hipGetDevicePropertiesR0600"})

    def test_non_callable_entries_do_not_satisfy_a_binding(self):
        for address in (0, 0x1400, 0x1700):
            with self.subTest(address=address):
                data = fixture()
                put(data, 0x504, "<I", address)
                self.assertEqual(pe_function_exports(data), {"hipInit"})

    def test_no_directory(self):
        data = fixture()
        put(data, OPTIONAL+112, "<Q", 0)
        self.assertEqual(pe_function_exports(data), set())

    def test_truncation_and_invalid_geometry(self):
        data = fixture()
        for size in (0, 63, PE+23, SECTIONS+79, 0x701, 0xc01):
            with self.subTest(size=size), self.assertRaises((RuntimeError, UnicodeError)):
                pe_function_exports(data[:size])
        for offset, fmt, value in (
                (PE+4, "<H", 0x14c), (PE+6, "<H", 97), (OPTIONAL, "<H", 0x10b),
                (OPTIONAL+108, "<I", 17), (OPTIONAL+116, "<I", 2**21),
                (DIRECTORY+20, "<I", 16385), (DIRECTORY+24, "<I", 16385),
                (0x702, "<H", 2), (0x604, "<I", 0x1400),
                (0x504, "<I", 0xffffffff), (0x604, "<I", 0xfffffff0),
                (SECTIONS+40+12, "<I", 0x1000)):
            with self.subTest(offset=offset, value=value), self.assertRaises((RuntimeError, UnicodeError)):
                data = fixture()
                put(data, offset, fmt, value)
                pe_function_exports(data)

    def test_unterminated_or_empty_name(self):
        for name in (bytes(256), b"x"*256):
            data = fixture()
            data[0x800:0x900] = name
            with self.subTest(name=name[:1]), self.assertRaises(RuntimeError):
                pe_function_exports(data)


if __name__ == "__main__":
    unittest.main()
