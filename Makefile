# Build with MinGW-w64 GCC. Native (MSYS2/MinGW shell) or cross (Linux):
#   make                       -> Mouthpiece.exe (64-bit)
#   make all                   -> Mouthpiece.exe, MouthpieceCLI.exe (64-bit) and MouthpieceVoiceHost32.exe (32-bit helper
#                                 that lets the 64-bit programs use 32-bit voices; keep it next to them)
#   make CROSS=i686-w64-mingw32-    builds the GUI/CLI as 32-bit instead (then no helper is needed)
CROSS ?= x86_64-w64-mingw32-
CC      = $(CROSS)gcc
WINDRES = $(CROSS)windres
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wno-unknown-pragmas -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
LDFLAGS = -municode -mwindows -static -s
LIBS    = -lsapi -lole32 -loleaut32 -luuid -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -lshell32

SRC = src/app.c src/tts_sapi.c src/util.c src/doc.c src/player.c src/preproc.c src/ui_cleanup.c src/ui_speech.c \
      src/pron.c src/speech.c src/utf8.c src/hash.c src/wav.c src/plat.c src/cache.c src/ir.c src/assemble.c \
      src/gen.c src/ui_generate.c src/ini.c src/chapters.c src/ui_nav.c src/ui_gutter.c src/config.c src/tts_remote.c src/ui_about.c src/pronscope.c src/rx.c src/find.c src/ui_find.c src/ui_profiles.c src/analyze.c src/ui_analyze.c src/ui_caption.c src/ui_caption_dlg.c src/ui_goto.c src/ui_hotkeys.c src/hotkeys.c src/navmath.c src/caplooks.c

Mouthpiece.exe: $(SRC) src/tts_engine.h src/util.h src/doc.h src/player.h src/preproc.h src/textutil.h src/ui_cleanup.h src/ui_speech.h \
  src/pron.h src/speech.h src/utf8.h src/hash.h src/wav.h src/plat.h src/cache.h src/ir.h \
  src/assemble.h src/gen.h src/ui_generate.h src/ini.h src/chapters.h src/ui_nav.h src/ui_gutter.h src/config.h src/tts_remote.h src/ui_about.h src/ui_about.h res/app.res.o
	$(CC) $(CFLAGS) $(SRC) res/app.res.o -o $@ $(LDFLAGS) $(LIBS)

res/app.res.o: res/app.rc res/app.manifest
	$(WINDRES) -O coff res/app.rc $@

# Unit tests for the pure-C core (doc.c). Runs on the host compiler, no Windows needed.
#   make test            (MinGW shell or Linux)
HOSTCC   ?= gcc
TEST_SAN ?= -fsanitize=address,undefined
test: tests/run_notes tests/run_find tests/run_doc tests/run_preproc tests/run_speech tests/run_cache tests/run_project tests/run_rx tests/run_pron2 tests/run_analyze tests/run_profiles tests/run_marks tests/run_navmath tests/run_hotkeys tests/run_caplooks
	./tests/run_marks
	./tests/run_navmath
	./tests/run_hotkeys
	./tests/run_caplooks
	./tests/run_profiles
	./tests/run_analyze
	./tests/run_notes
	./tests/run_find
	./tests/run_rx
	./tests/run_pron2
	./tests/run_doc
	./tests/run_preproc
	./tests/run_speech
	./tests/run_cache
	./tests/run_project
tests/run_doc: tests/test_doc.c src/doc.c src/doc.h src/textutil.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_doc.c src/doc.c -o $@
tests/run_preproc: tests/test_preproc.c src/preproc.c src/preproc.h src/doc.c src/doc.h src/textutil.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_preproc.c src/preproc.c src/doc.c -lm -o $@
tests/run_speech: tests/test_speech.c src/speech.c src/pron.c src/rx.c src/utf8.c src/doc.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_speech.c src/speech.c src/pron.c src/rx.c src/utf8.c src/doc.c -o $@
tests/run_cache: tests/test_cache.c src/hash.c src/wav.c src/plat.c src/cache.c src/ir.c src/assemble.c src/chapters.c src/ini.c src/speech.c src/pron.c src/rx.c src/utf8.c src/doc.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_cache.c src/hash.c src/wav.c src/plat.c src/cache.c src/ir.c src/assemble.c src/chapters.c src/ini.c src/speech.c src/pron.c src/rx.c src/utf8.c src/doc.c -lm -o $@

tests/run_project: tests/test_project.c src/ini.c src/chapters.c src/ir.c src/hash.c src/wav.c src/plat.c src/assemble.c src/speech.c src/pron.c src/rx.c src/utf8.c src/doc.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_project.c src/ini.c src/chapters.c src/ir.c src/hash.c src/wav.c src/plat.c src/assemble.c src/speech.c src/pron.c src/rx.c src/utf8.c src/doc.c -lm -o $@

all: Mouthpiece.exe MouthpieceCLI.exe MouthpieceVoiceHost32.exe

clean:
	rm -f tests/run_caplooks Mouthpiece.exe MouthpieceCLI.exe MouthpieceVoiceHost32.exe res/app.res.o tests/run_doc tests/run_preproc tests/run_speech tests/run_cache tests/run_project tests/run_rx tests/run_pron2 tests/run_notes tests/run_find tests/run_analyze tests/run_profiles tests/run_marks tests/run_navmath tests/run_hotkeys tests/run_caplooks

# Command line program: same engine as the GUI, console subsystem.
CLI_SRC = src/rx.c src/pronscope.c src/analyze.c src/cli.c src/tts_sapi.c src/tts_remote.c src/util.c src/doc.c src/preproc.c src/pron.c src/speech.c src/utf8.c src/hash.c \
          src/wav.c src/plat.c src/cache.c src/ir.c src/assemble.c src/gen.c src/ini.c src/config.c src/chapters.c
MouthpieceCLI.exe: $(CLI_SRC) src/*.h
	$(CC) $(CFLAGS) $(CLI_SRC) -o $@ -municode -static -s $(LIBS) -lm

# 32-bit voice helper (always 32-bit, whatever CROSS is).
MouthpieceVoiceHost32.exe: src/voicehost.c src/tts_sapi.c src/util.c src/*.h
	i686-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 \
	  src/voicehost.c src/tts_sapi.c src/util.c -o $@ -static -s -lsapi -lole32 -loleaut32 -luuid

tests/run_analyze: tests/test_analyze.c src/analyze.c src/pron.c src/rx.c src/utf8.c src/doc.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_analyze.c src/analyze.c src/pron.c src/rx.c src/utf8.c src/doc.c -o $@

tests/run_profiles: tests/test_profiles.c src/config.c src/ini.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_profiles.c src/config.c src/ini.c src/utf8.c src/preproc.c src/doc.c src/speech.c src/pron.c src/rx.c -lm -o $@

tests/run_marks: tests/test_marks.c src/chapters.c src/ini.c src/utf8.c src/doc.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_marks.c src/chapters.c src/ini.c src/utf8.c src/doc.c -o $@
tests/run_navmath: tests/test_navmath.c src/navmath.c src/doc.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_navmath.c src/navmath.c src/doc.c -o $@
tests/run_caplooks: tests/test_caplooks.c src/caplooks.c src/ini.c src/utf8.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_caplooks.c src/caplooks.c src/ini.c src/utf8.c -o $@

tests/run_hotkeys: tests/test_hotkeys.c src/hotkeys.c src/ini.c src/utf8.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_hotkeys.c src/hotkeys.c src/ini.c src/utf8.c -o $@

tests/run_rx: tests/test_rx.c src/rx.c src/rx.h src/textutil.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_rx.c src/rx.c -o $@
tests/run_pron2: tests/test_pron2.c src/pron.c src/rx.c src/pronscope.c src/speech.c src/chapters.c src/ini.c src/utf8.c src/doc.c src/*.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_pron2.c src/pron.c src/rx.c src/pronscope.c src/speech.c src/chapters.c src/ini.c src/utf8.c src/doc.c -lm -o $@

tests/run_find: tests/test_find.c src/find.c src/rx.c src/find.h src/textutil.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_find.c src/find.c src/rx.c -o $@

tests/run_notes: tests/test_notes.c src/preproc.c src/doc.c src/preproc.h src/textutil.h
	$(HOSTCC) -std=gnu11 -Wall -Wextra -g $(TEST_SAN) tests/test_notes.c src/preproc.c src/doc.c -lm -o $@
