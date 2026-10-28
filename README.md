# Mouthpiece

A native Windows text-to-speech workstation for listening to long texts: books, papers, scanned documents, web pages.
Plain C and the Win32 API, speech through the voices already installed in Windows (SAPI). No frameworks, no cloud, no account,
no telemetry. One small program, and everything stays on your machine.

Mouthpiece treats text-to-speech as a small compiler. Your text is never modified:

```
SOURCE  ->  NORMALIZED  ->  SPEECH TEXT  ->  AUDIO
(yours)     (cleanup)       (pronunciation,   (played live, or cached
                             numbers, rules)   and exported as WAV)
```

The window and the command line share one engine, so they behave the same.

## Download

Get the latest build from [`dist/`](dist/): **[Mouthpiece-win64.zip](dist/Mouthpiece-win64.zip)** (the three programs below), or the
`.exe` files individually. Unzip anywhere and run `Mouthpiece.exe`. No installer.

| File | What it is |
| --- | --- |
| `Mouthpiece.exe` | The program (64-bit) |
| `MouthpieceCLI.exe` | The command-line version, for batch jobs |
| `MouthpieceVoiceHost32.exe` | Small helper that lets the 64-bit programs use **32-bit voices**. Keep it next to the others |

Requires Windows 7 or later. The builds are unsigned, so Windows SmartScreen may ask you to confirm the first start
("More info" > "Run anyway").

> **Honest status:** all of this was written and cross-compiled on Linux. The core logic has automated tests (below), but the
> Windows interface (windows, menus, tray icon, global shortcuts, captions) has not yet been run by the author on every
> setup. Expect rough edges and please [open an issue](../../issues) when you find one.

## What it does

**Reading**
- Plays from the cursor, by sentence, paragraph or chapter; follows along in the text as it reads.
- Changing the speed while it is reading takes effect almost at once (it restarts at the word it is on).
- Lists every installed voice, including 32-bit ones; voice profiles save a voice with its rate and volume.
- Sleep timer, "stop at the end of this chapter".

**Messy input**
- Cleanup for PDF, OCR and web text: page numbers (including roman numerals), hyphenation, headers and footers, stray line breaks.
  A preview shows original and result side by side with the changes coloured.
- Chapter detection with manual correction; footnote and table handling.
- A *problem words* report: URLs, identifiers, odd numbers, acronyms, probable scanning errors and frequent names that a voice
  may get wrong, with a jump to each and a one-click pronunciation.

**Pronunciation**
- A dictionary with plain, whole-word and regex rules, scoped to the whole text, one chapter or a selection.
- Built-in speech rules for numbers, URLs and technical text. Hear a speech-text preview before you generate anything.
- Find and replace.

**A listening workstation**
- Typed marks: bookmark, revisit, important, unclear, question, doubt. Each has a coloured marker in the margin, an optional note,
  and can be edited or deleted with a right-click. Jump between marks; go back to where you were before a jump.
- Go to window: by percent, chapter, paragraph, sentence, mark, or a time skip (`+30s`, `-2m`).
- Lives in the **system tray**: minimize hides the window there, and the tray menu controls playback, chapters, marks, speed,
  profiles and the sleep timer.
- **Global shortcuts** that work while another program is in front, without hijacking your keyboard: one *leader key*
  (default Ctrl+Alt+Space) followed by a single key, plus optional direct shortcuts (Ctrl+Alt+P play/pause and so on) and media
  keys. A combination another program already owns is reported, not silently lost. See *Show all the shortcut keys* in the tray menu.
- **On-screen captions**: the sentence being read, in an always-on-top bar that lets clicks through. Choose font, size, colours,
  outline, text and background opacity and width; drag it anywhere; save your own looks. Long sentences page along with the
  words the voice is actually reaching.

**Audio output**
- Generate a single WAV or one WAV per chapter, with sentence, paragraph and chapter pauses, silence trimming and normalization.
- Audio is cached in small pieces keyed by the exact speech text, voice, speed and volume, so changing one paragraph re-speaks
  only that paragraph.
- Projects (`.ttsp`) keep text, chapters, marks, pronunciations and the cache together. A plain text file works too.

## Command line

```
MouthpieceCLI INPUT [options]        INPUT: a .txt file or a .ttsp project
MouthpieceCLI --list-voices

  --build                 synthesize (missing pieces only) and write one WAV       (default)
  --export-chapters DIR   one WAV per chapter
  --analyze               dry run: counts, chapters, cache status, estimated length
  --profile NAME   --voice NAME   --rate -10..10   --volume 0..100
  --sentence-pause MS  --paragraph-pause MS  --chapter-pause MS
  --normalize  --no-trim  --no-cleanup  --pronunciations FILE  --cache DIR  --output FILE
```

Exit code 0 success, 1 error, 2 bad arguments. The command line has no marks or captions.

## Where things are kept

Settings, the pronunciation dictionary and the cache live in `%APPDATA%\Mouthpiece` by default. Create a `MouthpieceData` folder next
to the program for a portable install, or pick another folder in *Tools > Data location*. The text you read is never changed.

## Build from source

Needs [MinGW-w64](https://www.mingw-w64.org/) GCC, either in an MSYS2 shell on Windows or as a cross compiler on Linux
(`x86_64-w64-mingw32-gcc`).

```
make            # Mouthpiece.exe (64-bit)
make all        # + MouthpieceCLI.exe and the 32-bit voice helper
make test       # unit tests of the pure-C core (host gcc, AddressSanitizer + UBSan; no Windows needed)
make clean
```

The core (document model, cleanup, speech rules, regex engine, cache, marks, navigation maths, shortcuts) is pure C with no Windows
types and is covered by about 700,000 automated checks. The Windows interface is compile-checked only.
[`ARCHITECTURE.md`](ARCHITECTURE.md) explains the pipeline, module by module, and lists the known limits.

```
src/     program source (pure-C core + ui_*.c Windows modules + app.c)
tests/   unit tests for the pure-C core
res/     resources and manifest
dist/    prebuilt Windows binaries
```

## Known limits

- Times for skips and Go to are estimates from the text length and speech rate, not measured audio.
- A live speed change and the captions' word following rely on estimates or on what each voice reports; a word may repeat or be
  skipped, and a voice that reports no word positions falls back to timed paging.
- "Stop at the end of the chapter" can let up to two already-queued sentences play.
- Programs in exclusive full-screen mode may hide the caption bar.

## License

GPL-3.0. See [LICENSE](LICENSE).
