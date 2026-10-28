# Mouthpiece - architecture and Milestone 1 notes

## 1. Module / file structure

```
src/
  tts_engine.h   backend-neutral API (voices, engine, speak, render). No SAPI types.
  tts_sapi.c     the only file that includes <sapi.h>; COM via lpVtbl
  util.h/.c      HRESULT/Win32 error text, text-file loader (UTF-8/16/ANSI, CRLF)
  app.c          Win32 UI: window, controls, playback state machine, export worker
res/app.rc, app.manifest   visual styles (comctl32 v6), system-DPI aware
Makefile
```
Planned growth (not created yet - no empty stubs):

| milestone | new modules |
|---|---|
| 2 | `doc.c` (document model), `segment.c` (paragraph/sentence splitter) |
| 3 | `preproc.c` (PDF/web cleanup, each rule togglable, produces a diff) |
| 4 | `pron.c` (dictionary, replace engine), pronunciation dialog in UI |
| 5 | `ir.c` (Speech IR), `hash.c` (FNV-1a/xxHash-style), `cache.c`, `assemble.c` |
| 6 | `project.c` (.ttsp text format), chapters/bookmarks |
| 7 | `analyze.c`, `cli.c` -> `MouthpieceCLI.exe`; GUI and CLI both link `core` (everything except `app.c`) |

Rule: `app.c` may call core modules; core modules never include `windows.h` UI headers or touch HWNDs.

## 2. Major structures and ownership

* `TTSVoice {id[256], name[128]}` - `id` is the SAPI token id (stable, used in cache keys); `name` is display only. `TTSVoiceList` is a heap array owned by the caller (`tts_voices_free`).
* `TTSEngine` (opaque) - owns one `ISpVoice` and one copy of the async text. Owned by the thread that created it; never shared.
* `TTSSpeechSettings {rate, volume}` - plain value, copied freely.
* `ExportJob` - heap object created by the UI thread, handed to one worker. UI owns text/path; worker owns `status`/`err` until it posts `WM_APP_EXPORT_DONE`; only `cancel` is touched concurrently (Interlocked). UI frees it after the thread is joined.
* All strings are UTF-16 (`wchar_t`) at the Win32/SAPI boundary. UTF-8 appears only in files (source, project, cache metadata) from Milestone 2 on.
* Error convention: functions return `TTSStatus` and fill a caller-supplied `wchar_t err[]` with a finished, user-presentable message.

## 3. Document-processing pipeline (Milestones 2-4)

```
SOURCE (immutable bytes/UTF-16, never edited by the pipeline)
  -> line/paragraph reconstruction + cleanup passes (each pass: in-range -> out-range + edit list)
  -> NORMALIZED (offset map back to SOURCE)
  -> sentence segmentation (abbreviation table + decimal/initial rules)
  -> speech normalization (Dr. -> Doctor, $25, years, URLs ...)
  -> pronunciation dictionary
  -> SPEECH TEXT per sentence
```
Every pass is a single linear scan over a region. Edits mark a paragraph dirty; only dirty paragraphs are re-run after a debounce. Each pass records edits so the preview can show ORIGINAL vs PROCESSED.

## 4. Speech IR (Milestone 5)

```
Segment { id; src_start; src_len; type;
          normalized_off/len; speech_text;
          voice_id; rate; volume; pitch;
          pause_before_ms; pause_after_ms; flags;
          synth_key (64-bit) }
```
Segments are sentences. A `SynthChunk` groups 1..n consecutive sentences with identical voice/settings for the engine; each chunk keeps the list of its segments.

## 5. Cache / invalidation

* Key = 128-bit hash (two independent 64-bit FNV-1a/xxHash runs) over `speech_text | voice_id | rate | volume | pitch | engine_id+version | output format`.
* **Speech-affecting** inputs are in the key. **Assembly-affecting** inputs (pauses, fades, trim, normalise gain) are *not*; they are applied in `assemble.c` on cached PCM.
* Pronunciation rules change `speech_text` only for sentences they match -> only those keys change. A voice change changes every key.
* Files: `cache/segments/<first 2 hex>/<hash>.wav` + one append-only `cache/index.log` (key, length, format, crc). The WAV is validated against index length/crc before use; mismatch -> "Cached audio segment is invalid. Regenerating...". Correctness never depends on filenames alone.
* Crash safety: each chunk is written to `*.tmp`, fsynced, renamed, then one index line appended.

## 6. Threading model

* **UI thread** (STA): all windows, playback engine, text edit control, timers.
* **Worker thread(s)** (MTA): own COM init + own `TTSEngine`; preprocessing, cache lookups, synthesis, cache writes. Communicate via `PostMessage(WM_APP_*)` plus a small mutex-protected job queue (Milestone 5). Cancellation = `volatile LONG` flag polled at <=100 ms plus `SPF_PURGEBEFORESPEAK`.
* Milestone 1 has exactly one worker (export) and one polling timer that exists only while speaking.

## 7. Win32 window/control architecture (Milestone 1)

One top-level window, no dialogs except common file dialogs. Children: STATIC labels, COMBOBOX (voice), two TRACKBARs (rate -10..10, volume 0..100), three BUTTONs, multiline EDIT (256M-char limit), status bar (state | char count). Layout is a single `layout(w,h)` function in DPI-scaled pixels. `IsDialogMessage` gives Tab/mnemonic navigation; `ES_WANTRETURN` keeps Enter in the editor. Shortcuts: F5 play/pause, Esc stop (not Space - it must type a space in the editor), Ctrl+O, Ctrl+E, Ctrl+A. Menus are built in code (no .rc menu).

## 8. Windows APIs used

COM: `CoInitializeEx`, `CoCreateInstance`. SAPI 5: `ISpVoice` (`SetVoice/SetRate/SetVolume/Speak(SPF_ASYNC|SPF_IS_NOT_XML)/Pause/Resume/WaitUntilDone/GetStatus/SetOutput`), `ISpObjectTokenCategory`+`IEnumSpObjectTokens`+`ISpObjectToken` (voice enumeration/lookup), `ISpStream::BindToFile` (WAV writer). Win32: windows/menus/accelerators, common controls (trackbar, status bar), `GetOpenFileNameW/GetSaveFileNameW`, `CreateThread`, `Interlocked*`, `FormatMessageW`, `WritePrivateProfileStringW` (tiny settings.ini in `%APPDATA%\TTSForge`).

## 9. Risky assumptions

1. **Not run on Windows yet.** It cross-compiles cleanly (MinGW-w64, `-Wall -Wextra`), but I had no Windows/Wine environment, so no runtime verification of SAPI behaviour.
2. **Voice visibility.** SAPI5 lists only SAPI voices. Windows 10/11 "Natural"/OneCore voices are generally *not* visible to it. A 64-bit exe also does not see 32-bit-only SAPI voices; build with `CROSS=i686-w64-mingw32-` if needed.
3. **WAV format.** M1 writes 22.05 kHz/16-bit/mono and relies on SAPI to convert if a voice differs. `ISpVoice` has no simple "native format" query; resolving this properly (probe via a null-output synthesis or voice token attributes) must happen before Milestone 5 because the format is part of the cache key.
4. **Live rate change** during speech is requested but I have not verified SAPI applies it mid-utterance; volume should.
5. **EDIT control** is fine for a few MB of text; a 500-page book is OK, but it will not scale to hundreds of MB and has no per-sentence highlight styling beyond selection. Milestone 2 may need a lighter custom text view or RichEdit.
6. **MAX_PATH** (260) paths only in M1.

## Review of Milestone 1 before continuing

* The engine API is the right shape, but `tts_speak_async()` takes the *whole text*. Milestone 2 must change playback to a sentence queue (speak one segment, advance on completion) - that is where sentence highlight, next/previous sentence and resume come from. Polling `WaitUntilDone` should then be replaced by SAPI end-of-stream notifications.
* `tts_render_wav()` writes straight to the final file. In Milestone 5 it becomes "render one chunk to a cache file", and the final WAV is produced by `assemble.c`, not by SAPI.
* Selection-or-all behaviour is already a stand-in for "play current segment/chapter".
* No tests exist yet because there is no document engine yet; they begin in Milestone 2 as a console test runner (`tests/run.exe`) linking only core modules.

---

# Milestone 2 - document model, segmentation, navigation, progress

## New / changed modules
| file | role |
|---|---|
| `src/doc.h/.c` | **Pure C** (no windows.h). `Document` = owned text + `Paragraph[]` + `Sentence[]`, all `(start,len)` spans into the untouched text. Paragraph + sentence segmentation, `doc_sentence_at()` (binary search), `doc_speech_text()` (collapses PDF-style line breaks for speaking without modifying the source). |
| `src/player.h/.c` | Sentence sequencer: keeps 3 sentences queued in the engine (gapless), reports which sentence is speaking. |
| `src/tts_engine.h` / `tts_sapi.c` | `tts_speak_async()` replaced by `tts_enqueue()` + `tts_queue_status()`; the engine owns the text copies of queued utterances until their stream finishes. |
| `tests/test_doc.c` | Console runner: your ugly-text cases, paragraph bookkeeping, offsets, long-sentence splitting, plus a 3000-iteration structural fuzz. `make test` (ASan+UBSan). |

## Segmentation rules (deterministic, O(n))
* Paragraph = run of non-blank lines; blank (whitespace-only) lines separate. Hard-wrapped lines stay inside one sentence span.
* Sentence ends at `. ! ? ...` (+ closing quotes/brackets) followed by whitespace and a **non-lower-case** word. No break for: titles/abbreviations (`Mr. Dr. Prof. vs. etc-lowercase ...`), initials (`J. S. Mill`), `e.g.`/`i.e.`, decimals/URLs (no whitespace after the dot), `Fig. 4`/`pp. 12`/`No. 5` (number follows), and dotted initialisms (`U.S.`) unless the next word is a common sentence opener (`The`, `He`, `It` ...).
* Sentences over 800 chars are split at `, ; :` then whitespace then hard cut, so cache units stay bounded.
* Known limits: CJK `。！？` and lone-`\r` line endings are not handled yet; `U.S. Economists ...` is not split (documented trade-off, predictable).

## Behaviour
* Edit -> 300 ms debounce -> full O(n) rebuild of the Document (incremental dirty-paragraph rebuild is a later optimisation; cost is a few ms per MB). Editing while speaking stops playback, since indices would go stale.
* The current sentence is highlighted using the editor's selection. **F5** plays from the sentence at the cursor to the end; **Ctrl+F5** plays the selection; **F6** one sentence; **Shift+F6** one paragraph; **F7** replays the current sentence (and continues); **Alt+Left/Right** previous/next sentence, **Alt+Up/Down** paragraph (also the Prev/Next buttons). While idle these just move the highlight. Esc stops.
* Status bar: `state | Para p/P  Sentence s/S  n% | chars`.
* Export now always means *whole document* (Ctrl+E) or *selection* (Ctrl+Shift+E) - never "whatever happens to be highlighted".

## Risks introduced / still open
1. **Unverified on Windows:** sentence-follow relies on `SPVOICESTATUS.ulCurrentStream` matching the stream numbers returned by `Speak()`. If the highlight lags or sticks on the first sentence when you run it, this is the first suspect; the fallback is SAPI end-of-stream events.
2. Playback "done" requires two consecutive idle polls (~200 ms) to avoid ending early before a freshly queued stream starts.
3. Sentence-to-sentence gaps depend on SAPI queueing; the deterministic pauses arrive with the assembler (Milestone 5).
4. Segmentation quality on real PDFs is limited until Milestone 3 repairs hyphenation/line joins *before* splitting (the spans already survive that change because they map to source offsets).
5. Player/engine logic has no automated test (needs a fake engine); only `doc.c` is covered.

---

# Milestone 3 - cleanup pipeline, offset map, preview

```
SOURCE (edit control, never modified)
  -> preproc.c  : lines -> detect headers / page numbers -> wrap detection -> one emit pass
  -> NORMALIZED text + offset map (+ optional change log)
  -> doc.c      : paragraphs / sentences (spans in normalized space)
  -> player / export
```

## New modules
| file | role |
|---|---|
| `src/preproc.h/.c` | **Pure C.** `preproc_run()` -> normalized text, `MapEntry[]` offset map, change log, counters. `preproc_norm_to_src / norm_end_to_src / src_to_norm`. |
| `src/textutil.h` | Locale-independent char classes shared by `doc.c` and `preproc.c`. |
| `src/ui_cleanup.h/.c` | Modal **Tools > Cleanup options and preview** (Ctrl+Shift+C): option checkboxes, ORIGINAL pane, PROCESSED pane, summary counts, and a clickable list of every individual change (click = jump to it in both panes). |
| `tests/test_preproc.c` | 580k checks: your spec cases, a synthetic 12-page "PDF book", offset-map round-trip property test, doc integration, randomized fuzz (ASan/UBSan). `make test` runs both suites. |

## Passes (all individually switchable, all deterministic, one linear pass + a sort of repeat candidates)
* **Repeated headers/footers** - short line (4-80 chars, >=3 letters), digits folded to `#` (so `42 THE ECONOMIC REVIEW` repeats), seen >=3 times, median gap >=8 lines, >=70% of gaps within 0.5x-2x of the median. `Chapter N / Part N / Book / Section / Act / Scene ...` are never treated as running heads.
* **Page numbers** - `Page 3`, `3 of 90`, `- 3 -` always; a **bare** number only with evidence (a neighbouring candidate 1-3 away and >=6 lines apart), so a lone `42` in prose, list numbers and number tables survive.
* **Join hard-wrapped lines** - only if the text *is* hard-wrapped (>=35% of lines sit in the top 15% of line width). Keeps breaks before list items, indented lines, ALL-CAPS headings, heading-like short lines, and after short sentence-ending lines. Paragraph-per-line text is left alone.
* **Hyphens** - `indus-\ntrial` -> `industrial`; `well-/self-/ex-/all-/half-/non-/quasi-/vice-`, one-letter prefixes (`e-mail`) and `Anglo-\nSaxon` keep the hyphen; mid-line hyphens are never touched; works across a removed running header.
* **Whitespace** (collapse/trim/blank runs), **Unicode** (ligatures, NBSP & other spaces, zero-width, soft hyphen, controls, `\r\n`/`\r`/`\f`/U+2028 line ends), **URLs** and **numeric citations** `[12] [1, 2] [3-5]` (off by default; also remove the orphaned space before punctuation).
* Footnotes / bibliography are **not implemented and not offered** (no fake controls).

## Offset map
Runs of `(norm_start, norm_len, src_start, src_len)`; 1:1 copies are merged into long runs, so the map is tiny (entries ~ number of edits, not characters). Lookups are binary searches. This is what lets the editor keep showing and highlighting the untouched SOURCE while the engine speaks the NORMALIZED text; caret and selection are mapped the other way. Removed text resolves to the next kept character.

## Behaviour changes
* Playback, sentence navigation, progress and **export** now all use the cleaned text. Export builds its text from the same sentences playback speaks (whole document, or the sentences overlapping the selection).
* Cleanup settings persist in `settings.ini` (`[Cleanup]`). Default: headers, page numbers, join, hyphens, whitespace, Unicode **on**; URLs and citations **off**. The master switch "Enable cleanup" turns everything off (text still gets `\n` line ends).
* Measured: ~47 ms for a 700-page, 1.9 M-char book (cleanup 42 ms + document 5 ms) on a desktop-class machine - fine for a 300 ms debounce, but it is still a full rebuild per edit burst.

## Known limits / risks
1. **UI untested on Windows** (compiled only): the dialog layout, list->pane jumping, and large-text behaviour of two read-only EDIT controls.
2. Heuristics can be wrong on unusual layouts: two-column PDFs, poems (joined if the text looks wrapped), scripts with repeated speaker names at regular intervals, running heads shorter than 4 characters. The preview exists precisely so these can be seen and the pass switched off. There is no per-item "undo this change" yet.
3. Hyphen keep-list is tiny by design (no dictionary); `Anglo-\nsaxon`-style lower-case compounds are repaired as one word.
4. Citation/URL removal is purely syntactic (author-year `(Smith, 2020)` is not touched).
5. Still a whole-document rebuild on each debounced edit (no dirty-region reuse yet).


---

# Milestone 3 follow-ups (your feedback)

* **Roman numerals** are now page numbers too: `Page iv`, `- xii -` are removed outright; a bare `i, ii, iii, iv / IX, X, XI` needs the same evidence as arabic numbers (a neighbour of the **same kind** 1-3 higher/lower, >= 6 lines away). Parsing is strict (canonical form only, one case) so `IIII`, `VX`, and words like `mix`/`did` are not numerals, and a lone `iv` or a list `I II III IV` is left alone. Arabic and roman evidence are kept separate (`iv` followed by `5` proves nothing).
* **Cleanup dialog panes follow each other**: both panes are RichEdit controls. A caret/selection change in ORIGINAL moves the caret/selection in PROCESSED (and vice versa) through the offset map; scrolling mirrors from whichever pane has focus (60 ms timer, only while the dialog is open). Switch it off with "Panes follow each other". Clicking an item in the change list jumps both panes to it.
* **Colour-coded changes**: red + strikethrough = removed (headers, page numbers, URLs, citations); amber = hyphen repaired; blue = line break joined; purple = Unicode fix. Colours appear in both panes where the change has a visible counterpart. The change log now also records line joins and Unicode fixes (sorted by source offset). Joins are coloured but not listed (there can be tens of thousands). RichEdit slows down with huge numbers of formatted runs, so colouring is capped at 4000 changes per group (removals/hyphens vs joins/Unicode); the summary line says when the cap applied.

---

# Milestone 4 - pronunciation dictionary, speech rules, previews

```
SOURCE -> (cleanup) -> NORMALIZED -> sentences -> [doc_speech_text: collapse whitespace]
       -> speech_render:  dictionary  >  built-in rules  >  copy      -> SPEECH TEXT -> engine
```

## New modules (all pure C except the two dialogs)
| file | role |
|---|---|
| `utf8.h/.c` | UTF-8 <-> wchar_t (UTF-16 or UTF-32), invalid input -> U+FFFD |
| `pron.h/.c` | Dictionary: add/find/set/remove, first-character bucket index (longest entry first), `pron_match`, `pron_signature` (fingerprint for the Milestone 5 cache key), UTF-8 text persistence |
| `speech.h/.c` | `speech_render()` (optionally returns the rewritten spans), `speech_sentence_text()` |
| `ui_speech.h/.c` | **Pronunciation editor** (Ctrl+Shift+P) and **Speech text preview** (Ctrl+Shift+V) |
| `util.c` | `util_read_file`, `util_write_file` (atomic: temp file + replace) |
| `player.c`, export | now speak `speech_sentence_text()` (dictionary + rules), the same text that export renders |

## Dictionary
* Entry = word/phrase -> replacement, flags: **match case**, **match inside words**. Default: case-insensitive, whole-word (a boundary is only required on a side where the entry starts/ends with a letter or digit, so `C++`, `.NET`, `Dr.` work). Longest entry wins; later entries never rescan earlier output.
* File: `%APPDATA%\TTSForge\pronunciations.txt`, UTF-8, one `flags<TAB>word<TAB>replacement` per line, `#` comments - inspectable and diff-able. Every edit is saved immediately; bad lines on load are skipped and reported.
* Editor: list of entries, Word / Replacement fields, **Speak Original** / **Speak Replacement** (uses the voice, rate and volume currently selected in the main window; no audiobook rebuild needed to test), Add / Update / Delete. Selecting a word in the main editor before Ctrl+Shift+P prefills it.
* **Not implemented (and not offered):** regex rules (no regex engine in the project) and chapter/selection scopes (chapters arrive in Milestone 6; selection scope needs anchors that survive edits). All entries are document-wide for now.

## Built-in speech rules (each switchable in the preview dialog)
* **Abbreviations** (case-sensitive, need the period): Mr. Mrs. Ms. Dr. Prof. Mt. Rev. Gen. Col. Lt. Capt. Sgt. Sen. Rep. Gov. Fig(s). Eq. Vol(s). Jr. Sr. Inc. Ltd. Corp. Co. vs. cf. approx. etc. pp., `e.g.` `i.e.`, and `No. 5` / `p. 12` only before a number. The full stop is kept when the abbreviation ends the sentence (`etc.`, `Inc.`).
* **Numbers**: cardinals up to 999 billion with thousands commas; decimals digit-by-digit (`3.14` -> three point one four); ordinals (`21st`); percent; currency `$ EUR GBP` (`$3.14` -> three dollars and fourteen cents, `$5 million` -> five million dollars); leading-zero and very long digit strings read digit by digit; `-5` -> minus five; **years**: 4 digits 1100-2099 are read as years only with context (in/since/during/from/of/..., month names, parentheses, `1980s`, or a year range, which reads "nineteen ninety to two thousand"), otherwise as a cardinal.
* **URLs / e-mail** -> "web address" / "email address"; **technical**: `C++`, `C#`, `F#`, `.NET`.
* Left exactly as written: times (10:30), dates (12/05/2020), section numbers (3.2.1), numbers glued to letters (x86, mp3, 5km), phone numbers.

## Speech text preview
Left: sentences that change (or all). Right: SOURCE sentence and SPOKEN text with the rewritten regions coloured - green = your dictionary, blue = built-in rules. Buttons: **Speak this**, **Add pronunciation...** (prefilled from the selection in the SOURCE pane, the list refreshes afterwards), **Use these settings**.

## Tests
`tests/test_speech.c`: number words, years/ordinals/decimals/currency/percent, abbreviations, URLs/e-mail/tech, "leave alone" cases, dictionary (case, whole-word, partial, longest-first, precedence over built-ins, not rescanned, index vs slow path, update/remove, duplicate/invalid rejection), serialization round trip with Greek/accents/an astral character, messy files (BOM, CRLF, bad lines), span bookkeeping, UTF-8 edge cases, and a 6000-iteration fuzz with span invariants. 18k checks under ASan/UBSan. Speed: 24,000 sentences with a 300-entry dictionary in ~46 ms.

## Risks / limits
1. **Windows UI untested** (compiled only): both new dialogs, RichEdit behaviour, speaking from inside a modal dialog.
2. Year-vs-cardinal is a heuristic (`1500 calories` after a non-context word reads "one thousand five hundred", `in 1500` reads "fifteen hundred"). Titles are English-only; the rules assume English voices.
3. The normalizer cannot know that a bare `St.` is Saint or Street, so it is left alone.
4. Dictionary replacements are plain text; SAPI phoneme/XML markup is deliberately not used (we send text with `SPF_IS_NOT_XML`).
5. The cache key (Milestone 5) must include `pron_signature()` plus the speech-rule options; per-sentence invalidation will compare the rendered speech text, so a dictionary edit only invalidates sentences whose text actually changed.

---

# Editor tweak: Home / End

Home and End in the text area now jump to the **start / end of the document** (Shift extends the selection; Ctrl+Home/End behave as before). Implemented by subclassing the edit control and presenting the key to it as Ctrl+Home/Ctrl+End, so scrolling and selection semantics are the control's own. **Tools > Home/End jump to document start/end** turns it off (line start/end again); the choice is saved. The status bar position updates after the jump.

---

# Milestone 5 - Speech IR, segment cache, incremental generation, assembly

```
Document -> speech rules -> chunks -> IR segments (key per segment)
                                          |
                      cache hit? --yes--> reuse
                           | no
                           v
              worker: SAPI -> tmp WAV -> validate -> cache (index.log)
                                          |
                                  ASSEMBLY (pauses/trim/normalize) -> output WAV
```
This replaces the old one-shot "Export WAV": File > **Generate audio / export WAV** (Ctrl+E whole document, Ctrl+Shift+E selection) opens the Generate dialog. Live playback is unchanged (it still streams through SAPI).

## New modules
| file | role | testable on any host |
|---|---|---|
| `hash.h/.c` | 128-bit non-cryptographic hash; text is hashed as UTF-8, so keys are platform independent | yes |
| `wav.h/.c` | RIFF/PCM header read/write on `FILE*`, CRC-32 | yes |
| `plat.h/.c` | the only OS-specific file code (mkdir -p, atomic replace, tree stats/delete); Win32 or POSIX | yes |
| `cache.h/.c` | persistent cache + `index.log` | yes |
| `ir.h/.c` | Speech IR: `IRSegment[]`, chunking, `ir_key()` | yes |
| `assemble.h/.c` | audio assembly, CRC verification | yes |
| `gen.h/.c` | background job: synthesize missing segments, repair, assemble | Windows |
| `ui_generate.h/.c` | Generate dialog | Windows |

## Speech IR segment
`{ id, first_sent..last_sent, para, para_end, src span (normalized offsets), text, key }`. A segment is a *synthesis chunk*: 1-8 consecutive sentences of the **same paragraph** (default 1; also capped at 700 characters). Logical sentences keep their own metadata (the chunk lists its sentence range), so navigation/highlighting stay sentence-based while synthesis can batch.

## Cache key - what invalidates what
```
key = hash( "tts-forge-seg-v1" | speech text | voice id | rate | volume | pitch | engine id | output sample rate )
```
| change | effect (verified by tests) |
|---|---|
| edit one sentence | exactly that segment (chunk) |
| add/change a pronunciation rule | only segments whose *rendered speech text* changes (rule for a word that never occurs: nothing) |
| speech-rule options (e.g. turn off number reading) | only segments the rules actually touched |
| voice / rate / volume / engine / output format | everything |
| chunk size | segments of the affected paragraphs (regrouping) |
| **sentence/paragraph pause, trim, normalize, pause scaling, output file** | **nothing - assembly only** |

Because the key uses the *final speech text*, dependency tracking needs no bookkeeping: if the text the engine would receive is identical, the audio is identical.

## Cache on disk (`%APPDATA%\TTSForge\cache`)
`segments\ab\<32 hex>.wav` (plain PCM), `tmp\`, and `index.log` (`key samples crc32 rate file_bytes`, plus `-key` tombstones). A hit requires an index entry **and** a file of exactly the recorded size. Writes go to `tmp`, are validated (PCM, 16-bit, mono, expected rate), CRC'd, renamed into place, then logged and flushed - a crash leaves at most an orphan file or a torn last line (ignored on load), never a half-valid entry. The CRC is checked again when audio is read for assembly; a mismatch is reported, the entry is removed, the segment is re-synthesized ("Cached audio segment is invalid. Regenerating...") and assembly retries (up to 3 times).

## Assembly (never calls the engine)
Concatenates cached PCM with deterministic silence (sentence pause between segments, paragraph pause after a paragraph's last segment), optional per-segment silence trimming (|sample| < 300, 5 ms margin kept), optional two-pass peak normalization (to ~ -1 dBFS, gain capped at 8x), optional pause scaling with speech rate (x2 at rate -10 ... x0.5 at +10). Output is written to `<file>.part` and renamed only on success; a failure never replaces an existing good file. A 4 GB WAV-size guard reports the limit before and during writing.

## Generate dialog
* **Analysis before any synthesis**: segments / already cached / to synthesize, duration (exact for cached segments, estimated for new ones), WAV size - recomputed when chunk size or pause settings change.
* Settings are grouped as "assembly only - never re-synthesizes" vs the chunk size.
* Progress bar and status ("Synthesizing segment N of M (x new, y reused)" then "Assembling"), **Stop** (finished segments stay cached; running it again continues where it left off - this is also the crash/resume path), cache size, Open cache folder, Clear cache (with confirmation).

## Threading
Worker thread, own COM apartment and `TTSEngine`; counters are `volatile LONG` read by a 200 ms UI timer; one `PostMessage` when done; the UI never touches the cache while a job runs. Closing the dialog is disabled while running (Stop first).

## Tests (`tests/test_cache.c`, 109 checks, ASan/UBSan)
Hash (determinism, field boundaries, similar-string family), IR invalidation matrix above (incl. 3-sentence chunks never spanning paragraphs, range builds equal full-build keys), cache (commit, wrong rate/stereo/garbage rejected, persistence across restart, torn index line, truncated file = miss, tombstones, missing file, 3000 entries with table growth, clear), assembly (exact lengths with pauses, changing pauses leaves the cache untouched, trim, normalize, **corruption detected and attributed to the right segment** in both passes, repair path, missing segment, cancel, empty, valid header), and the end-to-end scenario *build -> unchanged rebuild needs 0 work -> pronunciation fix regenerates only affected segments -> voice change regenerates all*.

## Risks / open items
1. **Windows side untested** (compiled only): the Generate dialog, the worker, and especially **many consecutive `SetOutput`/`BindToFile` renders on one SAPI voice**. If SAPI misbehaves there, the fallback is a fresh `ISpVoice` per segment (isolated to `gen.c`).
2. **Speed is unmeasured.** Per-segment file creation adds overhead versus one giant `Speak`; the payoff is that it happens once. Larger chunks (2-4 sentences) reduce overhead and often sound smoother, at the cost of coarser re-use.
3. Output is still fixed at 22.05 kHz / 16-bit / mono (SAPI converts). The format is part of the key, so a later "native format" change simply starts a new cache generation.
4. The key contains the voice **id**, not the voice's version: if Windows updates a voice, use **Clear cache**.
5. The cache is global per user (content-addressed, so it also dedupes across documents). Per-project cache folders arrive with the `.ttsp` project in Milestone 6. Two app instances writing the same cache at once are not coordinated.
6. Inside a multi-sentence chunk the engine decides pauses; between chunks they are deterministic. Chunk size 1 gives fully deterministic pause control.
7. Chapter pauses and per-chapter files are Milestone 6 (chapters), audio-cache playback (listening without exporting) is not implemented.

---

# Milestone 6 - chapters, bookmarks, projects, resume, per-chapter export

## New / changed modules
| file | role | host-testable |
|---|---|---|
| `ini.h/.c` | UTF-8 `[Section]` / `Key=Value` format (escapes `\\ \r \n`, first-appearance order). Used for the project file and settings. Replaces the ANSI Win32 `.ini` API, which would corrupt non-ASCII titles/paths/snippets | yes |
| `chapters.h/.c` | chapter detection, user corrections ("overrides"), bookmarks/resume anchors, chapter file names | yes |
| `ir.c`, `assemble.c` | segments carry `chapter` / `chapter_end`; a third pause level (**chapter pause**) | yes |
| `gen.c` | worker can write one WAV or **one WAV per chapter** | Windows |
| `ui_nav.h/.c` | **Navigator** (chapters + bookmarks, add/remove/rename headings, go to) and a small input box | Windows |
| `ui_generate.c` | chapter pause, "one WAV / one WAV per chapter", folder picker, overwrite confirmation | Windows |
| `app.c` | projects, resume, chapter/bookmark commands, settings moved to `ini.c` | Windows |

## Chapter detection (deterministic, paragraph-level)
A heading paragraph is short (<= 90 chars, <= 12 words), has no sentence punctuation, and is one of: `CHAPTER 7` / `Chapter Seven` / `Chapter 7: Title` / `PART I` / `BOOK TWO` (arabic, roman or number-word), a structural word (`Prologue Epilogue Preface Foreword Introduction Afterword Acknowledgments Conclusion Interlude Appendix A`), or a bare roman numeral (`IV.`). A heading with no title takes the next short unpunctuated paragraph as its title (`CHAPTER 1 - The Beginning`). `PART II` directly followed (<= 2 paragraphs) by a chapter heading is folded into that chapter; otherwise it stands alone. Ordinary prose that merely starts with those words (`Chapter 3 shows that...`, `Part of the problem...`, `Book three was long.`) is rejected (tested).

## User corrections never depend on position
Add heading / remove heading / rename are stored as **overrides keyed by the heading paragraph's text plus which occurrence of identical text** - so they survive edits elsewhere in the document (tested: inserting paragraphs above, two identical `Notes` headings). Editing the heading's own text detaches its override (by design: it is a different paragraph now). Overrides live in the project file.

## Bookmarks and the resume position
Both use one anchor: **semantic location** (chapter, paragraph in chapter, sentence in paragraph) + the sentence's opening words + which occurrence of those words. Resolution tries the structural location first (verified against the words), then searches by text (nth occurrence, else nearest), and reports "text not found" rather than pointing at the wrong place (tested: insert above, insert inside the chapter, delete). `Ctrl+B` toggles at the current sentence, `F2` / `Shift+F2` go to next/previous (wrapping). The resume position is saved on every project save/close and restored on open.

## Projects (`.ttsp`)
```
MyBook\
  book.ttsp            all settings + chapter fixes + bookmarks + position   (UTF-8 text)
  source.txt           the source text, UTF-8, exactly as in the editor (LF line ends)
  pronunciations.txt   project dictionary
  cache\               this project's audio cache (segments\ + index.log)
  export\              default output folder
```
* **File > New project from this text** (the current text, dictionary, bookmarks and chapter fixes move into it), **Open project**, **Save project (Ctrl+S)**, **Close project** (back to scratch mode with the global dictionary/cache/settings).
* Settings, chapter fixes, bookmarks and position are saved automatically (on change and on exit); the **text** is saved on Ctrl+S, and you are asked on close/open if it changed (title shows `*`).
* The last project is reopened at startup at the saved position; the audio cache lives in the project, so a half-finished 10-hour build resumes exactly where it stopped, even after moving the folder to another drive.
* `settings.txt` (in `%APPDATA%\TTSForge`) holds the scratch settings, the Home/End preference and the last project path. The old `settings.ini` is no longer read (one-time reset to defaults).

## Navigation and export
* Status bar: `Chapter 7/23   Para 32/180   Sentence 4/6   36%`. `Ctrl+PgUp/PgDn` previous/next chapter (first press goes to the chapter start), Speech > *Play this chapter only*, Tools > Navigator (`Ctrl+Shift+N`).
* **Generate audio**: whole document, selection, or **current chapter** (File menu). Chapter pause (default 1500 ms) in single-file mode; **per-chapter mode** writes `01 - Introduction.wav`, `02 - CHAPTER 1 - The Beginning.wav` ... (sequential numbers; invalid characters replaced, reserved device names escaped, length bounded, overwrite confirmed once per folder). Text before the first chapter becomes `01 - Beginning.wav`. Per-chapter files also sidestep the 4 GB WAV limit. Each file is written to `.part` and renamed only on success.

## Tests (new `tests/test_project.c`, 105 checks; plus the existing suites)
ini (unicode, multi-line, 3000-char values, escapes, BOM/CRLF/garbage, ordering), chapter detection (sample book with every heading style, false-positive prose, standalone part, empty), overrides (remove/add/rename, persistence, edits elsewhere, identical headings, forgetting manual headings), file names (invalid characters, reserved names, empty/dots, long, unicode), bookmarks (resolve, next/previous with wrap, insertion above and inside the chapter, deleted sentence, duplicate sentences, ini and resume-anchor round trip, stale-section cleanup), chapter-aware IR and assembly pauses.

## Risks / open items
1. **Windows UI untested** (compiled only): project dialogs, Navigator, input box, per-chapter generation, folder picker, startup auto-reopen, title/dirty tracking.
2. Chapter detection is English-oriented heuristics; two-column or decorative headings can be missed or over-detected - hence the Navigator corrections.
3. Bookmarks made in scratch mode are not persisted (a status message says so); create a project to keep them.
4. Scopes for pronunciation rules (chapter-level) and the rule system of spec section 18 (per-heading pause/rate rules) are still not implemented; chapters now exist, so they can be added next.
5. Moving a project means moving its folder; absolute paths are only used for the *last output* settings.
6. The Home/End change from the previous round also awaits a real-keyboard check (it presents the key to the edit control as Ctrl+Home/End).

---

## Feedback round after M6, and the command line

**Voices.** `tts_voices_enumerate` now reads four token categories (SAPI and OneCore, each under HKLM and HKCU) and de-duplicates by token id; OneCore voices are labelled `[OneCore]`. A 64-bit process sees only 64-bit voices and a 32-bit process only 32-bit ones, hence the second build (`Mouthpiece.exe`). Tools > Installed voices lists them, Tools > Rescan voices refreshes without restarting. Not every OneCore voice can be driven through SAPI; selection errors are reported when speaking.

**Where settings live.** Priority: a `MouthpieceData` folder next to the exe (portable) > the folder named in `%APPDATA%\TTSForge\datadir.txt` > `%APPDATA%\TTSForge`. Tools > Data location shows the active folder and why, opens it, or changes it (applies after restart; settings and dictionary are copied, never overwritten; the cache is not moved). Projects keep everything in their own folder regardless.

**Chapter and bookmark markers.** A plain EDIT control cannot colour text, so `ui_gutter.c` draws a strip beside it: orange `§` blocks for chapters, blue tabs for bookmarks, aligned using `EM_POSFROMCHAR`, clickable to jump. Marks are rebuilt after document rebuilds and bookmark changes. Between a keystroke and the 300 ms rebuild the marks can sit one line off.

**Blank document.** Ctrl+N (File > New blank document) and Close project both reset to an empty scratch document (asks before discarding unsaved scratch text).

**Home/End** jump to document start/end by default; the key is now `HomeEndJumpsToDocument` (default on), so old saved "off" values from the previous key name no longer apply.

**Shared configuration.** `config.c` (pure C) owns the `[Cleanup]`, `[SpeechRules]` and `[Audio]` keys and defaults for both programs.

**Command line (`MouthpieceCLI.exe`, `MouthpieceCLI.exe`).** `cli.c` uses the same cleanup, document, speech rules, dictionary, Speech IR, cache, `gen.c` worker and assembler as the GUI. `tts INPUT [--build | --export-chapters DIR | --analyze] [--output --voice --rate --volume --sentence-pause --paragraph-pause --chapter-pause --normalize --no-trim --no-cleanup --pronunciations --cache]`, `tts --list-voices`. INPUT is a .txt or a .ttsp project (its settings, chapter fixes, dictionary and cache are used). `--analyze` is a dry run: counts, chapters, cache hits, estimated length; nothing is synthesized.

**Not done / lost in a session reset.** The problem-detection analyzer (URLs, initials, acronyms, OCR errors per kind) and its GUI dialog were written but lost when the workspace was reset and were not rebuilt; `--analyze` is only the summary. Also unimplemented: regex rules, chapter/selection pronunciation scopes, footnote/table handling, voice profiles, find/replace.

**Verification status.** Unit suites (doc, preproc, speech, cache, project) pass; both 32- and 64-bit builds compile warning-free. Nothing Windows-side (dialogs, gutter alignment, voice scan, SAPI, the CLI) has been run on Windows by me.

---

## Renamed to Mouthpiece; 32-bit voices from the 64-bit program

**Name.** Program is Mouthpiece (`Mouthpiece.exe`, `MouthpieceCLI.exe`). Kept for compatibility: the `.ttsp` project extension, the cache key schema string (so existing audio caches stay valid), and settings in `%APPDATA%\TTSForge` are still used if `%APPDATA%\Mouthpiece` does not exist yet. Portable folder is now `MouthpieceData`.

**32-bit voices (`tts_remote.c`, `voicehost.c`, `hexcodec.h`).** A process can only load SAPI voices of its own bitness. The 64-bit programs therefore start `MouthpieceVoiceHost32.exe` (a 32-bit build of the same `tts_sapi.c`) and talk to it over stdin/stdout with one ASCII line per request (protocol in `tts_remote.h`; text is UTF-16 as hex). Behaviour:
- Enumeration lists local voices, then asks the helper and appends voices not already known, labelled `(32-bit)`.
- `tts_select_voice` tries the local engine first; if that fails it asks the helper. After that, enqueue, queue status, pause/resume/stop, settings and WAV rendering for that engine go to the helper. Everything above `tts_engine.h` (player, generation worker, CLI) is unchanged; each `TTSEngine` owns its own helper process.
- Cancelling a render kills the helper (a new one starts when needed); the partial file is deleted.
- Playback of a 32-bit voice makes one round trip per status poll (about every 100 ms).
- If the helper is missing, 32-bit voices are simply not listed.
Untested on a real Windows machine: the protocol, process start-up and any particular 32-bit voice.

**Finishing touches.** About is a scrollable FAQ (`ui_about.c`); window position/size/maximized state are remembered in settings.txt; a .txt or .ttsp can be dropped on the window.

# Pronunciation power, notes/tables, Find, profiles, problem words

## New modules (all pure C unless marked)
- `rx.c/h` - small regex engine compiled to a backtracking VM with a step limit (no catastrophic hangs). Syntax is documented in `rx.h`. `^`/`$` anchor on `\n` only.
- `pron.c` - entries gained flags (case, partial, regex) and an optional scope. A plain entry beats a regex at the same position; the same word may exist once as plain and once as regex. Regex replacement supports `$1..$9`.
- `pronscope.c` - scope strings: `chapter:<title>` and `range:A<US>n<US>B<US>m` (opening words of the first/last sentence plus occurrence number). `pron_resolve_scopes()` turns them into sentence ranges (`s_first`/`s_last`) after every document rebuild; `Speech.sentence1` carries the sentence index to the matcher.
- `find.c`, `ui_find.c` (Win32) - Find/Replace, plain or regex, one undo step for Replace all.
- `preproc.c` - new line passes: footnote markers/notes, tables (remove, or read row by row). The offset map back to the source is preserved.
- `config.c` - voice profiles stored in `[Profiles]` of settings.txt; used by `ui_profiles.c` and `MouthpieceCLI --profile`.
- `analyze.c`, `ui_analyze.c` - problem-word report. Classifies tokens (URL, e-mail, identifier, initials, unusual number, acronym, digit-in-word scanning error, frequent name >= 3 times, very long sentence); `handled` means a dictionary entry or the matching built-in rule covers it. `MouthpieceCLI --analyze` prints the same report.

## Cache compatibility
The cache key schema string and `.ttsp` extension are unchanged, so existing caches and projects still work. A pronunciation or option change alters the speech text, so only affected segments are re-spoken.

## Tests
12 suites, all ASan/UBSan clean: notes, find, rx, pron2, analyze, profiles, doc, preproc, speech, cache, project (`make test`).

## Known limits
- Never run on real Windows in this build environment: dialogs, gutter alignment, the 32-bit helper start-up and pipe protocol are compile-checked only.
- Footnote continuation lines (a note wrapped over several lines) are not followed; table detection is heuristic.
- A selection-scoped pronunciation stops matching if its anchor sentences are edited.
- Voice profiles cover voice, rate and volume; there are no per-chapter voices.
- Regex `^`/`$` match at `\n` only, so edited text with CRLF may need `\r?` in patterns.

# Listening workflow: marks, jumping, tray, global shortcuts, captions

## New pure-C modules (unit tested, ASan/UBSan)
- `chapters.c` - `Bookmark` gained `kind` (bookmark, revisit, important, unclear, question, doubt) and `note`. One list, one anchoring scheme (chapter/paragraph/sentence + opening words), so marks survive edits the same way bookmarks do. At most one mark of a kind per sentence; pressing the key again removes it. Persisted as `Kind=`/`Note=` keys (old projects load as plain bookmarks).
- `navmath.c` - back/forward position history, listening-time estimates (about 15 characters per second at rate 0, 7% per rate step), the Go to parser, and the caption chunker.
- `hotkeys.c` - the action list shared by tray menu, leader keys and direct shortcuts, the leader key table, and the `[Hotkeys]`/`[HotkeysDirect]` settings.

## Windows modules (compile-checked only)
- `ui_caption.c` - one layered, topmost, click-through, no-activate popup (`WS_EX_TRANSPARENT|NOACTIVATE`), placed on the monitor of the foreground window. Long sentences are paged on a timer proportional to chunk length.
- `ui_hotkeys.c` - shortcut settings window using the system hotkey control.
- `ui_gutter.c` - typed flags are coloured squares with a glyph; several on one line share the strip; right-click sends `WM_GUTTER_RCLICK`.
- `app.c` section "marks, jumping, tray, global shortcuts, captions" - tray icon (generated at runtime: idle/playing/paused), tooltip, menu, minimize/close to tray, sleep timer.

## Global shortcut design
`RegisterHotKey` is first come, first served system-wide. Only the LEADER combination is registered permanently. When it is pressed the single keys of `leader_keys()` are registered for a few seconds and released on timeout, Esc, or after a one-shot action; navigation keys keep the window open. If the leader is taken by another program, a few alternatives are tried and the outcome is reported. Direct shortcuts and media keys are optional and report failures.

## Second pass (captions, Go to, live speed, menus)
- `ui_caption.c` rewritten: per-pixel-alpha (`UpdateLayeredWindow`, premultiplied DIB), independent text/background opacity, outline, presets, move mode (`HTCAPTION` drag, double-click locks), position saved in settings.txt. `ui_caption_dlg.c` is the settings dialog with a live preview.
- `ui_goto.c` - Go to window (position, chapters, marks, skips, typed jumps via `goto_parse`).
- `WM_GUTTER_RCLICK` is sent for every marker kind, chapters and bookmarks included; the navigator has Edit note.
- Live speed: SAPI applies a rate only to utterances started later and the player queues about two sentences ahead, so `rate_changed_live` (debounced) restarts with `player_start_at(..., frac)`. The fraction is measured chars/sec times elapsed time; `text_resume_offset` snaps it to a word. Estimated, so a word may repeat or be skipped. The audio cache is unaffected (keyed by rate).
- `dlg_parent()`: a 1x1 transparent topmost tool window used as owner for dialogs when the main window is hidden, so tray-launched dialogs open.
- `hotkeys_help_text` / `hotkeys_help_dialog`: full list of program keys, leader keys, direct shortcuts and media keys.
- Menus regrouped (submenus for Tools: Captions, Tray and shortcuts, Listening); `sync_menu_checks` applies saved check states at start. Minimize goes to the tray by default (`SC_MINIMIZE`/`WM_SIZE`).

## Third pass
- Minimize-to-tray: the setting key is now `Tray/MinimizeHides` (default on). Earlier builds could have stored `MinimizeToTray=0` without the user choosing it, so the old key is ignored. If the tray icon is missing at minimize time it is added first.
- `hotkeys.c`: `hotkeys_default` now fills Ctrl+Alt direct shortcuts for every action and turns the media keys on. `DefaultsVersion` (2) in settings: older files get the defaults for every action stored as "none" and keep what the user set; from version 2 on "none" is written explicitly, so a removed default stays removed. A combination used twice is kept for the first action only. 643 checks.
- `caplooks.c` (pure, 13 checks): named looks (font, size, colours, opacities, outline, width; never position or on/off), `[CaptionLooks]` in settings.txt, up to 24, names unique ignoring case; the dialog has Save look / Delete look beside the list.
- Tray menu items carry their shortcut (`act_label`): the direct shortcut, else "leader, key".
- F5 / Play button (`play_from_cursor`): while reading, a caret on another sentence than the spoken one restarts there; otherwise pause/resume. The tray's Pause/Resume item is separate (`CMD_PLAY_TRAY`).
- Note on an unflagged passage adds a Revisit flag first, removed again if the note box is cancelled.

## Fourth pass (settings clobbering, shortcut display, caption sync)
- ROOT CAUSE of "no default shortcuts / minimize not working": `save_settings` calls `ext_to_ini`, and a project opening at start-up saved BEFORE `ext_init` had loaded the tray, shortcut and caption settings, so the all-zero state ("off", "none", font size 0) was written over the real values. `ext_to_ini` now does nothing until `X.ready`. Files already poisoned are repaired on load: shortcuts `DefaultsVersion` < 3 gets global shortcuts on, media keys on, the default leader key and a default shortcut for every action without one; a caption FontPt below 12 means "empty save, use the defaults"; the tray key is `HideOnMinimize` (default on).
- Shortcut display: `hotkeys_help_text` is a table (action, shortcut, key after the leader). `HotkeyCfg.failed[]` (runtime only) records which registrations another program won; tray items and the help table say "(in use)"; with global shortcuts off they say "(shortcuts off)". Tray items with a shortcut are owner-drawn (`add_act`, `tray_measure`, `tray_draw`) so the shortcut is grey.
- Shortcut settings dialog: columns (action, shortcut, key after leader), "Restore defaults" (`hotkeys_restore_defaults`).
- Caption sync: SAPI is asked for word-boundary events; `GetStatus().ulInputWordPos` (also reported by the 32-bit helper as a third value of its `T` reply) gives the character position of the word being spoken. `Player` turns it into a fraction of the sentence (`player_progress`, counting the resume offset after a live speed change) and `caption_progress` flips the page a few words BEFORE the next page is spoken. Voices that never report a position stay on the timed paging (now 12% fast). The poll timer is 40 ms (was 100). Fraction-to-text mapping is approximate because the spoken text (numbers expanded, pronunciations) differs from the displayed sentence.
- Not verified on Windows: whether each voice reports word positions, and whether `ulInputWordPos` counts characters (assumed) rather than bytes.

## Notes
- Flags use the sentence being spoken, or the one before it if the current one started less than 1.5 s ago (reaction lag).
- "Stop at end of chapter" lowers the player's last sentence; sentences already queued beyond it (at most two) may still be spoken.
- Data folder: a legacy `%APPDATA%\TTSForge` is renamed to `%APPDATA%\Mouthpiece` at start (used in place if the rename fails). The internal cache key schema string is unchanged on purpose so existing caches stay valid.
- Not verified on real Windows: tray behaviour, hotkey registration, overlay stacking over full-screen apps, dialogs. Direct-shortcut and caption fine tuning (width, hide delay) are in settings.txt only where no dialog exists.
