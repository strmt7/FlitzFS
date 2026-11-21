# Getting started with FlitzFS

[Documentation](../README.md) · [Features](features.md) · [Troubleshooting](troubleshooting.md)

FlitzFS has no public release or supported installer yet. It targets Windows 11
x64 and is not ready for everyday files or valuable data.

The current Windows application is an experimental editor and volume controller.
The Explorer variant attaches through a temporary folder. Developers can obtain
experimental builds using the
[build guide](../development/building.md).

## Experimental application workflow

The application currently supports this sequence:

1. Create a temporary test volume.
2. Save a new root-level text file under a new name.
3. Select, list and read the saved file.
4. Use **Optimize** for an explicit recompression operation.
5. Discard the test volume, then exit.

These are temporary memory contents: they do not survive process termination or
power loss. Keep original files outside the experiment. Closing the window can
hide it to the tray while the volume remains live; use the explicit discard and
exit controls to retire it.

The editor currently handles bounded text input. General file import, complete
Explorer integration, automatic GPU routing and unattended optimization are
still under development.

## Volume files in a qualified build

An Explorer build with qualified persistent backing offers **New volume file**
and **Open volume file**. Choose an unused filename to create a volume, or
select an existing FlitzFS volume to reopen it. **Open in Explorer** opens
the attached volume. Use **Detach and retain** before **Close volume file**.
Closing releases the volume file for a later open.
[Qualified backing and attachment behavior](../WINDOWS_VOLUME_APP.md#persistent-volume-files)

For implementation and qualification details, see the
[application contract](../WINDOWS_VOLUME_APP.md).
