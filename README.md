# winrtmidi

A minimal implementation of **WinRT MIDI** (`Windows.Devices.Midi`) for Wine/Proton, built on top of Wine's `winmm`.

Wine and Proton don't implement `Windows.Devices.Midi`. Apps that use it can't see any MIDI device. That includes anything built with the Rust crate [`midir`](https://github.com/Boddlnagg/midir) using its `winrt` backend. This DLL fills that gap.

It was written to get **ToneCommand** (Livtra's editor for the Ember Nanocore guitar pedal) working on Linux, but it should work for other apps that use the same API.

## What it implements

The subset that `midir` (backend `winrt`) uses:

- `Windows.Devices.Midi.MidiOutPort` / `MidiInPort`: `GetDeviceSelector`, `FromIdAsync`, `SendBuffer`, `SendMessage`, `MessageReceived`, `Close`. SysEx messages are reassembled on input.
- `Windows.Devices.Enumeration.DeviceInformation`: a proxy. MIDI selectors are handled here and everything else is forwarded to Wine's original `windows.devices.enumeration.dll`.
- `Windows.Storage.Streams.DataWriter` / `DataReader` / `Buffer` (with `IBufferByteAccess`), minimal.
- **USB hotplug:** Wine's `winmm` enumerates MIDI ports only once and can't tell when a device is unplugged. The DLL reads `/proc/asound/seq/clients` through `Z:`, hides ports that are gone, discards sends while the device is unplugged, and reopens the `winmm` handles when the port comes back.

Port names follow what Windows shows for a single USB-MIDI port: `"Nanocore - Nanocore MIDI 1"` becomes `"Nanocore"`. PipeWire and `Midi Through` ports are hidden.

## Building

You need `mingw64-gcc`. Building the fake ALSA device used by the tests also needs `alsa-lib-devel`.

```sh
make          # builds winrtmidi.dll
make tests    # tests/test.exe, tests/test2.exe, tests/fakedev
```

## Installing into a prefix

Close the app first.

```sh
./winrtmidi.sh install   ~/Games/myprefix
./winrtmidi.sh uninstall ~/Games/myprefix   # restores the original registry entries

# to use a Proton build's wine instead of the system one:
PROTON_FILES=~/.local/share/lutris/runners/wine/proton-cachyos-11.0-x86_64/files ./winrtmidi.sh install ~/Games/myprefix
```

`install` copies the DLL to `system32` and registers the classes under `HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId`. A Proton prefix update can reset that registry key. If that happens, run `install` again.

## Environment variables

| Variable | Effect |
|---|---|
| `WINRTMIDI_LOG` | Log path, as a Windows path. Default: `C:\winrtmidi.log`. The log rotates at 4 MB. |
| `WINRTMIDI_VERBOSE=1` | Logs every MIDI message, in hex. |
| `WINRTMIDI_ALLPORTS=1` | Doesn't hide PipeWire or `Midi Through` ports. |
| `WINRTMIDI_RAWNAMES=1` | Uses the raw `winmm` names, without shortening them. |

## Tests

- `tests/test.exe` makes the same WinRT calls `midir` does: it enumerates ports, opens them, sends a 3000-byte SysEx and receives it back. By default it uses `Midi Through`, so run it with `WINRTMIDI_ALLPORTS=1`. Use `TEST_PORT=<name>` to pick a different port.
- `tests/fakedev` plus `tests/test2.exe` test hotplug. `fakedev` creates an ALSA port named `FakeNano` that echoes back whatever it receives. Kill it and start it again while `test2.exe` (or `test2.exe reopen`) is running.

## Limitations

- The device has to be plugged in **when the app starts**, because that's the only time Wine enumerates MIDI devices.
- Reconnecting assumes the device comes back with the same ALSA seq client number. That number comes from the sound card's index. If it changes, the log shows a warning. You can pin the index, for example with `options snd-usb-audio vid=0x33c3 pid=0x1301 index=2` in `/etc/modprobe.d/`.
- `DeviceWatcher` isn't implemented for MIDI selectors; those calls are forwarded to Wine. Apps that poll `FindAllAsync` work fine.
- Don't flash firmware through Wine. Use real Windows or a VM.

Code comments and log messages are in Portuguese (pt-BR).

## AI disclosure

This code was written with Claude (Anthropic) through Claude Code, under my direction, and I tested it with my own hardware.

## License

MIT, see [LICENSE](LICENSE).
