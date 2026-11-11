"""Bounded static AMD64 PE function-export inspection; never load the module."""
from check_portable_cpu import MIB, number, require, span


def pe_function_exports(data: bytes) -> set[str]:
    require(0 < len(data) <= 128 * MIB, "runtime PE size budget")
    require(span(data, 0, 2) == b"MZ", "expected DOS header")
    pe = number(data, 0x3c, "<I")
    require(span(data, pe, 4) == b"PE\0\0" and number(data, pe + 4, "<H") == 0x8664,
            "expected AMD64 PE")
    sections = number(data, pe + 6, "<H")
    optional_size = number(data, pe + 20, "<H")
    optional = pe + 24
    require(1 <= sections <= 96 and optional_size >= 120, "PE header budget")
    span(data, optional, optional_size)
    require(number(data, optional, "<H") == 0x20b and
            1 <= number(data, optional + 108, "<I") <= (optional_size - 112) // 8,
            "invalid PE32+ directory count")
    section_start = optional + optional_size
    span(data, section_start, sections * 40)

    def locate(value: int, size: int) -> tuple[int, int]:
        candidates = []
        for index in range(sections):
            base = section_start + index * 40
            virtual = number(data, base + 12, "<I")
            raw_size = number(data, base + 16, "<I")
            raw_offset = number(data, base + 20, "<I")
            if virtual <= value and value - virtual <= raw_size and size <= raw_size - (value - virtual):
                candidates.append((raw_offset + value - virtual, number(data, base + 36, "<I")))
        require(len(candidates) == 1, "PE RVA has no unique file-backed section")
        span(data, candidates[0][0], size)
        return candidates[0]

    export_rva = number(data, optional + 112, "<I")
    export_size = number(data, optional + 116, "<I")
    if not export_rva and not export_size:
        return set()
    require(export_rva != 0 and 40 <= export_size <= MIB and export_rva + export_size <= 2**32,
            "PE export directory budget")
    directory, _ = locate(export_rva, export_size)
    functions = number(data, directory + 20, "<I")
    names = number(data, directory + 24, "<I")
    require(0 < functions <= 16384 and names <= 16384, "PE export inventory budget")
    addresses, _ = locate(number(data, directory + 28, "<I"), functions * 4)
    if not names:
        return set()
    name_table, _ = locate(number(data, directory + 32, "<I"), names * 4)
    ordinals, _ = locate(number(data, directory + 36, "<I"), names * 2)
    seen, executable = set(), set()
    for index in range(names):
        name_rva = number(data, name_table + index * 4, "<I")
        value = bytearray()
        for offset in range(256):
            start, _ = locate(name_rva + offset, 1)
            byte = data[start]
            if byte == 0:
                break
            value.append(byte)
        else:
            raise RuntimeError("PE export name exceeds budget")
        require(bool(value), "empty PE export name")
        name = value.decode("ascii")
        require(name not in seen, "duplicate PE export name")
        seen.add(name)
        ordinal = number(data, ordinals + index * 2, "<H")
        require(ordinal < functions, "PE export ordinal outside function table")
        function_rva = number(data, addresses + ordinal * 4, "<I")
        # Forwarders require a different module; data and empty exports are not
        # callable bindings. The worker requires direct executable entries.
        if not function_rva or export_rva <= function_rva < export_rva + export_size:
            continue
        _, flags = locate(function_rva, 1)
        if flags & 0x20000000:
            executable.add(name)
    return executable
