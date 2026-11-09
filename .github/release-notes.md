Mouthpiece, a native Windows text-to-speech workstation (plain C + Win32, uses the voices installed in Windows).

**Download `Mouthpiece-win64.zip`**, unzip anywhere and run `Mouthpiece.exe`. Keep the three programs in one folder.

| File | What it is |
| --- | --- |
| `Mouthpiece.exe` | The program (64-bit) |
| `MouthpieceCLI.exe` | Command-line version |
| `MouthpieceVoiceHost32.exe` | Helper that lets the 64-bit programs use 32-bit voices |

### Windows / antivirus
These programs are **not code-signed**, so SmartScreen may show "Windows protected your PC" (More info > Run anyway), and some
antivirus products may flag a new unsigned program by guesswork. Mouthpiece does not install anything, has no network code and no
registry code, and does not use keyboard hooks. This build adds version information, an icon, a complete manifest and ASLR/DEP
linking to look like what it is. If something still flags it, please open an issue with the antivirus name and detection name, and
report it as a false positive to the vendor (Microsoft: https://www.microsoft.com/wdsi/filesubmission).

### Status
Written and cross-compiled on Linux. The core logic has automated tests; the Windows interface has had limited real-world testing, so
expect rough edges. See the README for features and known limits.
