#include "ui_about.h"
#include <stdlib.h>
#include <string.h>

#define ID_TEXT 1
#define ID_OK 2

static const wchar_t *kText =
L"MOUTHPIECE\r\n"
L"A native Windows text-to-speech workstation for turning long, messy text into clean audio.\r\n"
L"\r\n"
L"HOW IT WORKS\r\n"
L"Your text goes through four stages:  SOURCE  >  NORMALIZED  >  SPEECH TEXT  >  AUDIO.\r\n"
L"  - SOURCE is what you typed or opened. Mouthpiece never changes it.\r\n"
L"  - NORMALIZED is the source after cleanup (page numbers, running headers, broken hyphens, URLs and so on).\r\n"
L"  - SPEECH TEXT is what the voice is actually given: pronunciation dictionary, numbers, abbreviations.\r\n"
L"  - AUDIO is generated one segment at a time and cached.\r\n"
L"Tools > Cleanup options and Tools > Speech text preview show you each stage.\r\n"
L"\r\n"
L"FREQUENTLY ASKED QUESTIONS\r\n"
L"\r\n"
L"Why does changing a pause or normalization not re-generate the audio?\r\n"
L"Pauses, silence trimming and normalization are applied when the audio is assembled, not when it is spoken. "
L"Only changes that affect speech (voice, rate, volume, the text itself, the dictionary, speech rules) make "
L"segments stale. Generating again only re-speaks the segments that changed.\r\n"
L"\r\n"
L"Where are my settings, dictionary and audio cache?\r\n"
L"Tools > Data location shows the folder and lets you open or move it. By default it is %APPDATA%\\Mouthpiece. "
L"Make a folder named MouthpieceData next to the program for portable mode. A project (.ttsp) keeps its own "
L"text, settings, dictionary, cache and exports in its own folder.\r\n"
L"\r\n"
L"What is a project?\r\n"
L"File > New project from this text saves the text, chapter corrections, bookmarks and your reading position "
L"in a folder, so you can close the program and resume exactly where you were. Project files end in .ttsp "
L"(the extension is unchanged from earlier versions, so old projects still open). "
L"Close project and File > New blank document (Ctrl+N) give you an empty document.\r\n"
L"\r\n"
L"A voice I installed is missing from the list.\r\n"
L"Tools > Installed voices shows what was found and Tools > Rescan voices looks again. 32-bit voices work in "
L"this 64-bit program through a small helper, MouthpieceVoiceHost32.exe, which must stay in the same folder as "
L"Mouthpiece.exe; they are labelled (32-bit). Voices marked [OneCore] are the newer Windows voices, and some "
L"of those cannot be used through SAPI. Voices from other speech systems (for example browser or cloud voices) "
L"are not SAPI voices and cannot be listed.\r\n"
L"\r\n"
L"Does Mouthpiece need the internet?\r\n"
L"No. Everything runs on your computer with the voices installed in Windows.\r\n"
L"\r\n"
L"How do chapters and bookmarks work?\r\n"
L"Chapter headings are detected automatically (\"Chapter 3\", \"PART TWO\", numbered headings and so on) "
L"and shown as orange \xA7 marks in the strip beside the text; bookmarks are blue tabs. Click a mark to jump to it. "
L"Tools > Navigator lets you add, remove or rename chapters when detection gets it wrong, and these fixes "
L"survive editing. Audiobook export can write one file per chapter.\r\n"
L"\r\n"
L"Why do Home and End jump to the start and end of the document?\r\n"
L"It is a choice for long reading; turn it off in Tools if you prefer line start/end.\r\n"
L"\r\n"
L"Is there a command line version?\r\n"
L"Yes: MouthpieceCLI.exe. \"MouthpieceCLI --help\" lists everything. It uses the same engine, settings, "
L"dictionary and cache, can read a .txt file or a .ttsp project, can export one file per chapter, and "
L"--analyze reports counts, chapters and the estimated length without synthesizing anything.\r\n"
L"\r\n"
L"Can I fix how a word is pronounced?\r\n"
L"Yes: Tools > Pronunciation editor. An entry can match whole words or parts of words, can respect case, and can be a "
L"regular expression (with $1, $2 in the replacement). You can limit an entry to the chapter at the cursor or to the "
L"selected sentences; a selection is remembered by the words that open its first and last sentence, so it stops "
L"matching if you edit those sentences. Tools > Problem words lists URLs, e-mail addresses, acronyms, initials, "
L"identifiers, unusual numbers, probable scanning errors and frequently used names, with Go to and Add pronunciation.\r\n"
L"\r\n"
L"How are footnotes and tables handled?\r\n"
L"Cleanup has options to remove footnote markers and notes, to remove tables, or to read simple tables row by row. "
L"Detection is by pattern, so check the speech text preview (Ctrl+Shift+V). Your source text is never changed.\r\n"
L"\r\n"
L"What are voice profiles?\r\n"
L"Tools > Voice profiles stores a voice with its rate and volume under a name. MouthpieceCLI --profile NAME uses the same list.\r\n"
L"\r\n"
L"Find and replace\r\n"
L"Ctrl+F opens it; F3 and Shift+F3 repeat the search. It can use regular expressions and replace all matches in one undoable step.\r\n"
L"\r\n"
L"How do I mark passages while I listen?\r\n"
L"Ctrl+B bookmarks; Alt+1 revisit, Alt+2 important, Alt+3 unclear, Alt+4 question, Alt+5 doubt. A flag goes on the passage being "
L"spoken (if it began less than a second and a half ago, on the one before it, since you react a moment late). Pressing the same "
L"key again removes it. Each kind has its own coloured square in the strip beside the text; right-click one to edit its note, change "
L"its type or delete it. Ctrl+M adds a note; Mark > Manage lists everything. F2 / Shift+F2 jump to the next / previous mark. "
L"Marks are saved with a project, and find their sentence again by its opening words if the text moves.\r\n"
L"\r\n"
L"How do I jump around?\r\n"
L"Alt+Backspace goes back to where you were before your last jump (Alt+Shift+Backspace goes forward again), Ctrl+, and Ctrl+. skip "
L"30 seconds, Ctrl+G opens Go to (40%, c12 chapter, p300 paragraph, s1200 sentence, m3 mark, +30s, -2m). Times are estimates "
L"from the text length and speech rate, not measured audio.\r\n"
L"\r\n"
L"Using it while I work in other programs\r\n"
L"F5 plays from the cursor: if you click somewhere else while it is reading, F5 restarts there, otherwise it pauses or resumes. A note on an unflagged passage first adds a Revisit flag.\r\n"
L"\r\n"
L"The tray icon is the control panel: click it to play/pause, double-click to open the window, right-click for movement, flags, marks, "
L"speed, voice profiles, a sleep timer, captions and settings. Minimizing hides the window in the tray (changeable in the Tools menu). Global keys would clash "
L"with other programs, so only one is always claimed, the LEADER key (default Ctrl+Alt+Space, changeable). Press it, then within a few seconds "
L"a single key: Space play/pause, arrows move, R I U Q X flag, B bookmark, J back, [ ] chapters, +/- speed, C captions. "
L"The single keys are released again afterwards. Tools > Global shortcut settings lets you add direct shortcuts and media keys, "
L"and tells you if another program already owns a combination.\r\n"
L"\r\n"
L"On-screen captions\r\n"
L"Tools > Captions > Caption settings opens a dialog for font, size, bold, text and background colours, outline, separate text and background "
L"opacity, width and position. \"Save look...\" stores the current font, size, colours, opacity and outline under a name (up to 24); \"Delete look\" removes one. Built-in looks (high contrast, dark, light, large print) are in the same list. Use Move captions to drag the bar anywhere with the mouse; "
L"double-click it to lock it again. The bar stays on top of other windows and otherwise lets clicks through. Long sentences are paged by the words the voice is actually reaching (with a timed estimate as the fallback for voices that do not report positions), a little ahead of the speech. Programs in exclusive full-screen mode may hide it.\r\n"
L"\r\n"
L"Changing the speed while it is reading\r\n"
L"The voice restarts at the word it is on (found by estimate), so the new speed is heard almost at once; it may repeat or skip a word.\r\n"
L"\r\n"
L"Flags and bookmarks\r\n"
L"Right-click any marker in the left strip, or a chapter or bookmark in the navigator, to change its type, edit its note or delete it. "
L"Go to (Ctrl+G) is a window with the position, chapters, marks and time skips; it also accepts typed jumps.\r\n"
L"\r\n"
L"Which global keys exist?\r\n"
L"Tray > Show shortcut keys lists the program keys, the leader key and every key available after it, and your direct shortcuts and media keys "
L"with their current state. The tray menu shows the shortcut in grey next to each item, with \"(in use)\" if another program owns it or \"(shortcuts off)\" when global shortcuts are switched off. Show all the shortcut keys is a table of every action with its shortcut and its key after the leader key. \"Restore defaults\" in Shortcut settings puts the defaults back. Out of the box the program sets Ctrl+Alt shortcuts (P play/pause, S stop, arrows move, PageUp/PageDown chapters, B R I U Q X flags, M next mark, + - speed, C captions, W window) and the media keys; change or clear any of them in Shortcut settings. A combination another program already owns is reported at start. Shortcut settings also opens from the tray while the window is hidden.\r\n"
L"\r\n"
L"Does it re-generate everything when I change one thing?\r\n"
L"No. Audio is cached in small pieces (a few sentences each) keyed by the exact speech text, voice, speed and volume. Changing text "
L"re-speaks only the pieces that changed. Marks, pauses and normalization never re-speak anything.\r\n"
L"\r\n"
L"KEYS\r\n"
L"F5 play / pause      Ctrl+F5 play selection      F6 this sentence      Shift+F6 this paragraph      F7 replay\r\n"
L"Alt+Left/Right sentence      Alt+Up/Down paragraph      Ctrl+PgUp/PgDn chapter      Esc stop\r\n"
L"Ctrl+O open text      Ctrl+N new      Ctrl+S save project      Ctrl+E generate audio      Ctrl+Shift+E generate selection\r\n"
L"Ctrl+B bookmark      F2 / Shift+F2 next / previous bookmark      Ctrl+Shift+N navigator\r\n"
L"Ctrl+F find and replace      F3 / Shift+F3 find next / previous\r\n"
L"Alt+Backspace jump back      Ctrl+G go to      Ctrl+, / Ctrl+. skip 30 s      Ctrl+M note on mark      Ctrl+Shift+L captions\r\n"
L"Ctrl+Shift+C cleanup      Ctrl+Shift+P pronunciation      Ctrl+Shift+V speech text preview\r\n"
L"You can also drag a .txt file or a .ttsp project onto the window.\r\n";

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_OK || LOWORD(wp) == IDCANCEL) { DestroyWindow(h); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_SIZE: {
        RECT r;
        GetClientRect(h, &r);
        HWND t = GetDlgItem(h, ID_TEXT), b = GetDlgItem(h, ID_OK);
        MoveWindow(t, 8, 8, r.right - 16, r.bottom - 56, TRUE);
        MoveWindow(b, r.right - 98, r.bottom - 38, 90, 28, TRUE);
        return 0;
    }
    case WM_DESTROY: PostMessageW(h, WM_NULL, 0, 0); break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static BOOL CALLBACK font_cb(HWND c, LPARAM f) { SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

void about_dialog(HWND parent, HFONT font, int dpi)
{
    WNDCLASSEXW wc;
    if (!GetClassInfoExW(GetModuleHandleW(NULL), L"MouthpieceAbout", &wc)) {
        memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MouthpieceAbout";
        RegisterClassExW(&wc);
    }
    int w = MulDiv(680, dpi, 96), h = MulDiv(620, dpi, 96);
    RECT pr;
    GetWindowRect(parent, &pr);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VISIBLE;
    EnableWindow(parent, FALSE);
    HWND hw = CreateWindowExW(WS_EX_CONTROLPARENT, L"MouthpieceAbout", L"About Mouthpiece - questions and answers", style,
                              pr.left + ((pr.right - pr.left) - w) / 2, pr.top + ((pr.bottom - pr.top) - h) / 2, w, h,
                              parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!hw) { EnableWindow(parent, TRUE); return; }
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", kText, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE |
                    ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, hw, (HMENU)(INT_PTR)ID_TEXT, GetModuleHandleW(NULL), NULL);
    CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, hw,
                    (HMENU)(INT_PTR)ID_OK, GetModuleHandleW(NULL), NULL);
    EnumChildWindows(hw, font_cb, (LPARAM)font);
    RECT r;
    GetClientRect(hw, &r);
    SendMessageW(hw, WM_SIZE, 0, MAKELPARAM(r.right, r.bottom));
    SendMessageW(GetDlgItem(hw, ID_TEXT), EM_SETSEL, 0, 0);
    SetFocus(GetDlgItem(hw, ID_OK));
    MSG m;
    while (IsWindow(hw) && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hw, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}
