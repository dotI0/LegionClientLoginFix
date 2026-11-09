# Legion 7.3.5 login fix

The repair supports only these Windows x64 Legion clients:

- **7.3.5.26972**: tested in-game.
- **7.3.5.26365**: code signatures and downloader layout verified in the
  running client; in-game testing is pending.

The launcher selects a profile by its runtime signatures, not by the
executable name, version metadata, or file hash. The DLL checks the selected
layout again before changing anything. Other builds may start but will not
receive the fix.

No client executable, credentials, logs, or server files are included.

## Start

- Keep `StartWoWWithLoginFix.exe` and `WowLoginRepair.dll` together. The
  launcher looks for `Wow-64.exe` next to itself or one directory above. If
  neither is found, it asks you to select the client executable. WoW starts
  with its own directory as the working directory.
- By default, the launcher leaves the cache alone, uses no priority override,
  passes no extra WoW arguments, and writes no log.
- Start `Wow-64.exe` normally to play without the fix. Neither the original
  client executable nor its `version.dll` is replaced.

## Arguments

- `-log`: Write `WowLoginLauncher-<launcher PID>.log` beside the DLL. It
  records the startup steps and why the fix could not be loaded or activated,
  even if the launcher closes before you can read its console. It also records
  the WoW PID and the DLL log filename on success. The DLL writes its own
  `WowLoginRepair-<WoW PID>.log` once its diagnostics start; a
  `repair ... result=detached` entry records a repaired watcher. The folder
  must be writable. Do not include generated logs when distributing the package.
- `-clear-cache`: Remove only the `Cache` directory next to the selected
  client. If the directory exists, wait one second after removal and verify
  it is gone before starting WoW. A missing cache needs no delay. Junctions
  and other reparse points are refused.
- `-high`: Start WoW with Windows' High process priority. This is a launcher
  option, not a WoW argument.
- `--client "path\to\Wow-64.exe"`: Select a client executable explicitly,
  including one with a different filename.

All other arguments, including `-console`, are passed to WoW in the order
given. Quote arguments containing spaces. Launcher options can be mixed with
WoW arguments; everything after `--` is passed to WoW as-is.

```cmd
StartWoWWithLoginFix.exe
StartWoWWithLoginFix.exe -console -high -log -clear-cache
StartWoWWithLoginFix.exe --client "D:\Games\Legion\Wow-Test.exe" -log -- -console
```

## Build

Close the launcher and WoW before rebuilding. In an x64 Native Tools Command
Prompt for Visual Studio, run `source\build.cmd`. It builds both the EXE
and DLL and embeds `source\icon.ico` in the launcher. Use the rebuilt files
together; do not mix them with older versions. No third-party build
dependencies are required.
