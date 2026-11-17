# winrtmidi

A minimal implementation of **WinRT MIDI** (`Windows.Devices.Midi`) for Wine/Proton, built on top of Wine's `winmm`.

Wine and Proton don't implement `Windows.Devices.Midi`. Apps that use it can't see any MIDI device. That includes anything built with the Rust crate [`midir`](https://github.com/Boddlnagg/midir) using its `winrt` backend. This DLL fills that gap.

It was written to get **ToneCommand** (Livtra's editor for the Ember Nanocore guitar pedal) working on Linux, but it should work for other apps that use the same API.

## Why this exists

The LIVTRA NANOCORE is a small multi-effect guitar pedal. Over USB it reports itself as `33c3:1301 Ember Nanocore` and is class-compliant: it shows up as a regular USB sound card plus a USB-MIDI port, with no driver needed. On Linux the kernel picks it up right away (`snd-usb-audio`, ALSA seq client `Nanocore`). What's missing is the editor. Livtra ships **ToneCommand** only for Windows and macOS ([downloads](https://livtramusic.com/downloads)), plus an iPad app.

Under Wine/Proton, ToneCommand starts but never finds the pedal. The reason:

- The UI is Flutter. The device logic lives in a Rust library (`livtra.dll`), and it talks MIDI through [`midir`](https://github.com/Boddlnagg/midir) 0.10 using the **WinRT** backend, `Windows.Devices.Midi`. That's the modern Windows MIDI API, not the classic `winmm` `midiIn*/midiOut*` one.
- Wine implements `winmm` MIDI on top of ALSA, but it has **no `Windows.Devices.Midi` at all**. The [`dlls/`](https://gitlab.winehq.org/wine/wine/-/tree/master/dlls) tree has `windows.devices.enumeration`, `.bluetooth` and `.usb`, but no `.midi`. A [Wine fork issue](https://github.com/giang17/wine/issues/7) points out the same gap. Wine's `DeviceInformation` is also only partly implemented: `Name` and `CreateFromIdAsync` are stubs, and `DataReader` isn't registered.
- So `MidiOutPort.GetDeviceSelector()` / `DeviceInformation.FindAllAsync()` return nothing, so the pedal never shows up in the app.

This DLL plugs that hole without touching Wine itself. It's an ordinary PE DLL that registers itself as the activatable class for those WinRT types and translates every call to `winmm`, which Wine already supports well.

## How it works

```
ToneCommand (Flutter UI)
  └─ livtra.dll (Rust) ── midir, backend winrt
        │  RoGetActivationFactory("Windows.Devices.Midi.MidiOutPort", …)
        ▼
  combase: HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId\<class>\DllPath
        ▼
  winrtmidi.dll  ← this project
    ├─ MidiOutPort / MidiInPort        → midiOutOpen/midiOutLongMsg, midiInOpen + callback
    ├─ DeviceInformation (proxy)       → MIDI: enumerates winmm ports; other: original Wine DLL
    └─ DataWriter / DataReader / Buffer
        ▼
  Wine winmm → winealsa.drv → ALSA sequencer → snd-usb-audio → pedal
```

1. **Activation.** WinRT apps don't link to an import library. They ask `combase` for an activation factory by class name, and `combase` looks up the DLL in the registry. `install.reg` points `MidiOutPort`, `MidiInPort`, `DeviceInformation`, `DataWriter` and `DataReader` at `winrtmidi.dll`. `uninstall.reg` restores Wine's original entries.
2. **Enumeration.** `GetDeviceSelector()` returns the same AQS strings real Windows does, including the MIDI interface-class GUIDs. When `DeviceInformation.FindAllAsync*` gets one of those selectors, the DLL builds the list from `midiInGetDevCaps`/`midiOutGetDevCaps` and gives each port an id like `\\?\WINEMIDI#OUT#<n>#<winmm name>`. Any other selector (Bluetooth, audio, …) goes to Wine's original `windows.devices.enumeration.dll`, which the DLL loads and wraps.
3. **Output.** `SendBuffer` takes the bytes from the `IBuffer` (through `IBufferByteAccess`). Each SysEx is sent as one `midiOutLongMsg`, which waits for `MHDR_DONE`. Everything else is split into `midiOutShortMsg` calls, with running status handled.
4. **Input.** `midiInOpen` uses a callback that only pushes into a queue. A worker thread drains it, reassembles SysEx messages that ALSA delivers in chunks, wraps each message in a `MidiMessageReceivedEventArgs`, and calls the app's `TypedEventHandler`. A 3000-byte SysEx loopback arrives intact, as a single message.
5. **Async.** Every `IAsyncOperation` comes back already completed. The `Completed` handler fires immediately and `GetResults` works. That's all `midir`'s blocking `.get()` needs.
6. **Hotplug.** `winealsa` enumerates ports only once, at startup, and doesn't notice when a device goes away. A PE DLL can't call ALSA, but Wine maps `Z:` to `/`. So the DLL reads `/proc/asound/seq/clients` (at most every 300 ms), keeps a per-port "epoch" that bumps each time the port reappears, and:
   - hides absent ports from `FindAllAsync`, so the app notices the disconnect;
   - quietly drops writes while the port is gone, because returning an error could crash the app;
   - when the epoch changes, closes and reopens the `winmm` handle, which makes Wine subscribe to the ALSA port again. This works whether the app keeps its port objects or recreates them; with ToneCommand, it recreates them.

The implementation is plain C against hand-written vtables, with no WinRT headers or C++/WinRT. `src/genguids.py` and `src/piid.py` compute the parameterized interface IIDs (`IAsyncOperation<…>`, `TypedEventHandler<…>`, `IVectorView<…>`) the same way the Windows runtime does.

## ToneCommand on Linux: notes

- **Use Proton.** Tested with Proton-CachyOS 11.0 through `umu-run`. On my system, Fedora's wine-staging 11.20 crashed the Flutter app at startup, intermittently, with an `ntdll` `create_view` assertion.
- Plug the pedal in **before** starting the app. After that, unplugging and replugging is handled (see above).
- The pedal is also a USB sound card. ToneCommand opens a WASAPI output stream (through `cpal`), which goes through Wine's normal PulseAudio/PipeWire path. This project doesn't touch audio.
- Don't flash firmware through Wine. The upgrader uses DFU over libusbK; use a real Windows machine or a VM with USB passthrough.
- Livtra's RESONA OMNI capture app uses classic `winmm` MIDI and runs under Proton without this DLL.

## Other people working on this

The NANOCORE has a small but active community that doesn't want to depend on the official app. As far as I could find, none of them took this route; they rewrote the editor from scratch:

- [**dvilelaf/nanocore-controller**](https://github.com/dvilelaf/nanocore-controller): a CLI, a Python library and a local web editor over USB MIDI and BLE-MIDI. It runs on Linux and its protocol was reverse engineered from ToneCommand. It also documents using the pedal as a USB sound card on the PC.
- [**lucanenni/livtra-nanocore-editor**](https://github.com/lucanenni/livtra-nanocore-editor): an unofficial browser editor (Web MIDI / Web Bluetooth, Chromium) for the NanoCore and Mosaic, with protocol notes taken from captures of ToneCommand's traffic.

Those are the better choice if you want something native and open. This project is for when you want the **official** app: its full feature set and its preset formats, without keeping a Windows machine around. It's also a general fix for any other Windows app that uses WinRT MIDI under Wine, such as apps built with `midir`'s winrt backend.

This repository contains no Livtra code, binaries, keys or protocol details. It only implements a public Windows API.

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
