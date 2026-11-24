# Legion 7.3.5 login fix

> [!WARNING]
> Windows Smart App Control must be disabled for this application to work properly.
> 
> How to disable Smart App Control on Windows 11:
> 1. Open the Start Menu and search for Windows Security.
> 2. Navigate to App & browser control.
> 3. Click on Smart App Control settings.
> 4. Select Off to disable Smart App Control.
> 5. Confirm the change if prompted.
> 6. Restart the application.

> [!CAUTION]
> **Antivirus detections**
>
> This launcher intentionally injects the included fix DLL into `Wow-64.exe` and patches the running client to fix the login issue. These techniques are also used by malware, which can cause antivirus software to flag the launcher.
>
> If your antivirus blocks it, you may need to add an exception for the launcher and fix DLL. Only allow files from a source you trust. Do not disable your antivirus or exclude your entire game folder.
>
> DLL injection is intentional here and does not, by itself, mean the application is malware. The complete source code is available in this repository. If you do not trust the provided binaries, you are welcome to review the code and build the application yourself.

The repair supports only these Windows x64 Legion clients:

- **7.3.5.26972**: tested in-game.
- **7.3.5.26365**: tested in-game.

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
- `-50-chars`: Opt in to the 50-character selection-screen limit on build
  **7.3.5.26972** or **7.3.5.26365**. Requires the matching server-side support
  described below.
  This option does not enable logging and is not passed to WoW.
- `--client "path\to\Wow-64.exe"`: Select a client executable explicitly,
  including one with a different filename.

All other arguments, including `-console`, are passed to WoW in the order
given. Quote arguments containing spaces. Launcher options can be mixed with
WoW arguments; everything after `--` is passed to WoW as-is.

```cmd
StartWoWWithLoginFix.exe
StartWoWWithLoginFix.exe -console -high -log -clear-cache
StartWoWWithLoginFix.exe --client "D:\Games\Legion\Wow-Test.exe" -log -- -console
StartWoWWithLoginFix.exe -50-chars -log
```

## Optional 50-character mode

The login repair still supports both client builds listed above. The optional
`-50-chars` extension supports **7.3.5.26972** and **7.3.5.26365**, with a
separate runtime-signature profile for each build. If the requested extension
cannot start, the launcher explains why;
the already active login repair remains active.

This mode maintains the stock GlueXML `MAX_CHARACTERS_PER_REALM` value at 50
instead of 16, using the client's UI thread. It leaves other nonstandard values
alone. It does not add a selection cache, change character GUIDs, or replace
the character-sorting logic. No executable or GlueXML file is rewritten.
Restart WoW without `-50-chars` to return to the stock interface.

The server must separately permit the additional characters. For example:

```ini
CharactersPerAccount = 50
CharactersPerRealm = 50
```

The server's character-reordering packet must also support up to 50 entries;
changing only its configuration is not sufficient on an unmodified core.
Its position values are byte-sized sort keys (for example 10, 20, 30), not
roster indexes from 0 to 49. A server-side index check would reject valid
reordering requests.
The extension does not bypass server checks or create any characters itself.

With `-log`, the launcher records whether the UI-thread override was installed
or why it failed. That confirms installation, not a complete 50-character
gameplay test. Test creation beyond 16, scrolling, reordering, restoring and
logging in on a test account first. Also test returning with more than 16
characters using the unmodified client before relying on that fallback.

## Build

Close the launcher and WoW before rebuilding. In an x64 Native Tools Command
Prompt for Visual Studio, run `source\build.cmd`. It builds both the EXE
and DLL and embeds `source\icon.ico` in the launcher. Use the rebuilt files
together; do not mix them with older versions. No third-party build
dependencies are required.

The new launcher requires the versioned DLL entry point so an older DLL cannot
silently acknowledge `-50-chars` without applying it. The updated DLL retains
the old entry point for existing launchers, with the original logging-only
behavior.
