// USF Maker - Windows front end.
// Copyright (C) 2026 Nekokabu.  GPL-2.0-or-later, see LICENSE.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/ripper.h"
#include "engine/statefile.h"
#include "player.h"
#include "resource.h"
#include "strings.h"

using namespace usfm;

namespace {

// ---------------------------------------------------------------- helpers
std::wstring widen(const std::string& s, UINT cp = CP_UTF8)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring fmt(const wchar_t* format, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, format);
    const int n = vswprintf(buf, sizeof(buf) / sizeof(buf[0]), format, ap);
    va_end(ap);
    if (n < 0)
        buf[sizeof(buf) / sizeof(buf[0]) - 1] = 0;
    return buf;
}

std::wstring time_text(double seconds)
{
    return widen(format_time_short(seconds));
}

// Header names are plain ASCII on western releases and Shift-JIS on some
// Japanese ones.
std::wstring rom_name_text(const std::string& raw)
{
    if (raw.empty())
        return T(S_ROM_NONAME);
    bool high = false;
    for (unsigned char c : raw)
        if (c >= 0x80)
            high = true;
    return widen(raw, high ? 932 : CP_UTF8);
}

// ------------------------------------------------------------ identifiers
enum : int {
    IDC_ROM_LABEL = 2000, IDC_ROM_EDIT, IDC_ROM_OPEN, IDC_ROM_INFO,
    IDC_GAME_LABEL, IDC_GAME_EDIT, IDC_ARTIST_LABEL, IDC_ARTIST_EDIT,
    IDC_TIME_LABEL, IDC_TIME_COMBO, IDC_PRESS, IDC_SETTINGS,
    IDC_START, IDC_STOP, IDC_PROGRESS, IDC_STATUS,
    IDC_LIST,
    IDC_PLAY, IDC_RENAME, IDC_DELETE, IDC_SELALL, IDC_IMPORT, IDC_SAVE, IDC_HELPBTN,
    IDC_LOG,
};

enum : UINT {
    WM_APP_LOG = WM_APP + 1,   // lParam: std::wstring*
    WM_APP_PROGRESS,           // lParam: RipProgress*
    WM_APP_SONG_ADDED,         // lParam: SongInfo*
    WM_APP_SONG_REMOVED,       // wParam: id
    WM_APP_RIP_DONE,
    WM_APP_IMPORT_DONE,        // wParam: added, lParam: failed
    WM_APP_PLAY_ENDED,         // wParam: id, lParam: 1 = failed
    WM_APP_AUTO,
};

const double kTimeChoices[] = { 60, 180, 300, 600, 1800, 0 };
const StrId kTimeLabels[] = { S_TIME_1, S_TIME_3, S_TIME_5, S_TIME_10, S_TIME_30, S_TIME_FOREVER };

// ------------------------------------------------------------- app state
class UiListener : public RipListener {
public:
    HWND wnd = nullptr;
    bool verbose = false;   // /verbose: show the engine's own (English) diagnostics too
    void on_log(const std::string& s) override
    {
        if (!verbose)
            return;
        PostMessageW(wnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(widen(s)));
    }
    void on_progress(const RipProgress& p) override { PostMessageW(wnd, WM_APP_PROGRESS, 0, (LPARAM) new RipProgress(p)); }
    void on_song_added(const SongInfo& s) override { PostMessageW(wnd, WM_APP_SONG_ADDED, 0, (LPARAM) new SongInfo(s)); }
    void on_song_removed(int id) override { PostMessageW(wnd, WM_APP_SONG_REMOVED, (WPARAM)id, 0); }
};

struct App {
    HINSTANCE inst = nullptr;
    HWND wnd = nullptr;
    HFONT font = nullptr;
    HFONT font_bold = nullptr;
    int dpi = 96;

    std::shared_ptr<Rom> rom;
    Path rom_path;
    std::unique_ptr<Ripper> ripper;
    UiListener listener;
    RipOptions opt;
    bool full_rom = false;

    std::thread rip_thread;
    std::thread task_thread;
    bool ripping = false;
    bool stopping = false;
    bool importing = false;
    Player player;

    // unattended run: /auto <rom> <out folder> <seconds>
    bool auto_mode = false;
    std::wstring auto_rom, auto_out;
    double auto_seconds = 60;
    int exit_code = 0;
};

App g;

HWND ctl(int id) { return GetDlgItem(g.wnd, id); }
int S(int v) { return MulDiv(v, g.dpi, 96); }

void set_status(const std::wstring& text)
{
    SetWindowTextW(ctl(IDC_STATUS), text.c_str());
}

void add_log(const std::wstring& line)
{
    HWND e = ctl(IDC_LOG);
    int len = GetWindowTextLengthW(e);
    if (len > 60000) {
        // drop the older half
        SendMessageW(e, EM_SETSEL, 0, len / 2);
        SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"");
        len = GetWindowTextLengthW(e);
    }
    std::wstring text = line;
    text += L"\r\n";
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

void error_box(const std::wstring& text)
{
    if (g.auto_mode) {
        add_log(text);
        return;
    }
    MessageBoxW(g.wnd, text.c_str(), T(S_ERR_TITLE), MB_OK | MB_ICONWARNING);
}

std::wstring get_text(int id)
{
    HWND h = ctl(id);
    const int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

// --------------------------------------------------------------- list view
int list_find(int song_id)
{
    HWND lv = ctl(IDC_LIST);
    const int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; i++) {
        LVITEMW it = {};
        it.mask = LVIF_PARAM;
        it.iItem = i;
        if (ListView_GetItem(lv, &it) && (int)it.lParam == song_id)
            return i;
    }
    return -1;
}

int list_id_at(int row)
{
    LVITEMW it = {};
    it.mask = LVIF_PARAM;
    it.iItem = row;
    if (!ListView_GetItem(ctl(IDC_LIST), &it))
        return 0;
    return (int)it.lParam;
}

int list_selected_row()
{
    return ListView_GetNextItem(ctl(IDC_LIST), -1, LVNI_SELECTED);
}

void list_set(int row, int col, const std::wstring& text)
{
    ListView_SetItemText(ctl(IDC_LIST), row, col, const_cast<wchar_t*>(text.c_str()));
}

void list_fill_row(int row, const SongInfo& s)
{
    list_set(row, 1, widen(format_time_short(s.length_seconds)) + (s.fade_seconds > 0 ? fmt(L" + %.0f", s.fade_seconds) : std::wstring()));
    list_set(row, 2, T(s.kind == SongKind::Looping ? S_KIND_LOOP : s.kind == SongKind::Ending ? S_KIND_END : S_KIND_UNKNOWN));
    list_set(row, 3, s.kind == SongKind::Looping ? fmt(L"%.2f + %.2f", s.intro_seconds, s.loop_seconds) : std::wstring());
    if (s.origin == SongOrigin::Imported)
        list_set(row, 4, T(S_ORIGIN_IMPORT));
    else
        list_set(row, 4, fmt(T(s.origin == SongOrigin::BootIdle ? S_ORIGIN_IDLE : S_ORIGIN_EXPLORED), time_text(s.found_at_seconds).c_str()));
    list_set(row, 5, fmt(L"%u Hz", s.sample_rate));
    std::wstring note;
    if (s.stalls)
        note += T(S_NOTE_STALL);
    if (!s.hle_ok) {
        if (!note.empty())
            note += L" / ";
        note += T(S_NOTE_LLE);
    }
    list_set(row, 6, note);
}

void list_add(const SongInfo& s)
{
    HWND lv = ctl(IDC_LIST);
    const std::wstring title = widen(s.title);
    LVITEMW it = {};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = const_cast<wchar_t*>(title.c_str());
    it.lParam = (LPARAM)s.id;
    const int row = ListView_InsertItem(lv, &it);
    if (row < 0)
        return;
    ListView_SetCheckState(lv, row, TRUE);
    list_fill_row(row, s);
}

std::vector<int> list_checked_ids()
{
    std::vector<int> ids;
    HWND lv = ctl(IDC_LIST);
    const int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n; i++)
        if (ListView_GetCheckState(lv, i))
            ids.push_back(list_id_at(i));
    return ids;
}

// ------------------------------------------------------------ ui state
void update_buttons()
{
    const bool have_rom = g.rom && !g.rom->empty();
    const bool busy = g.ripping || g.importing;
    const int rows = ListView_GetItemCount(ctl(IDC_LIST));
    const bool sel = list_selected_row() >= 0;
    EnableWindow(ctl(IDC_ROM_OPEN), !busy);
    EnableWindow(ctl(IDC_START), have_rom && !busy);
    EnableWindow(ctl(IDC_STOP), g.ripping && !g.stopping);
    EnableWindow(ctl(IDC_TIME_COMBO), !busy);
    EnableWindow(ctl(IDC_PRESS), !busy);
    EnableWindow(ctl(IDC_SETTINGS), !busy);
    EnableWindow(ctl(IDC_PLAY), sel || g.player.playing());
    EnableWindow(ctl(IDC_RENAME), sel);
    EnableWindow(ctl(IDC_DELETE), sel && !busy);
    EnableWindow(ctl(IDC_SELALL), rows > 0);
    EnableWindow(ctl(IDC_IMPORT), have_rom && !busy);
    EnableWindow(ctl(IDC_SAVE), rows > 0 && !busy);
    SetWindowTextW(ctl(IDC_PLAY), T(g.player.playing() ? S_PLAY_STOP : S_PLAY));
}

void stop_playback()
{
    g.player.stop();
}

// ----------------------------------------------------------------- ROM
bool load_rom(const std::wstring& path)
{
    if (g.ripping || g.importing) {
        error_box(T(S_ERR_BUSY));
        return false;
    }
    if (g.ripper && !g.ripper->songs().empty() && !g.auto_mode) {
        if (MessageBoxW(g.wnd, T(S_ASK_REPLACE_ROM), T(S_APP_TITLE), MB_YESNO | MB_ICONQUESTION) != IDYES)
            return false;
    }
    auto rom = std::make_shared<Rom>();
    std::string err;
    if (!rom->load(Path(path), err)) {
        error_box(fmt(T(S_ERR_ROM), widen(err).c_str()));
        return false;
    }

    stop_playback();
    if (g.rip_thread.joinable())
        g.rip_thread.join();
    g.ripper.reset(new Ripper);
    g.rom = rom;
    g.rom_path = Path(path);
    g.ripper->set_rom(rom);
    g.ripper->set_listener(&g.listener);
    ListView_DeleteAllItems(ctl(IDC_LIST));

    const RomInfo& ri = rom->info();
    SetWindowTextW(ctl(IDC_ROM_EDIT), path.c_str());
    SetWindowTextW(ctl(IDC_ROM_INFO),
                   fmt(T(S_ROM_INFO), rom_name_text(ri.internal_name).c_str(), widen(ri.game_code).c_str(), widen(ri.cic).c_str(),
                       ri.pal ? L"PAL" : L"NTSC", (double)ri.size / 1048576.0)
                       .c_str());
    SetWindowTextW(ctl(IDC_GAME_EDIT), g.rom_path.stem().wstring().c_str());
    SendMessageW(ctl(IDC_PROGRESS), PBM_SETPOS, 0, 0);
    set_status(T(S_STATUS_READY));
    update_buttons();
    return true;
}

void browse_rom()
{
    wchar_t file[MAX_PATH * 4] = L"";
    std::wstring filter = T(S_ROM_FILTER);
    std::replace(filter.begin(), filter.end(), L'|', L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn))
        load_rom(file);
}

// ---------------------------------------------------------------- search
// Switches the progress bar between "busy" animation and a normal bar.
void progress_marquee(bool on)
{
    HWND pb = ctl(IDC_PROGRESS);
    LONG_PTR style = GetWindowLongPtrW(pb, GWL_STYLE);
    if (on) {
        SetWindowLongPtrW(pb, GWL_STYLE, style | PBS_MARQUEE);
        SendMessageW(pb, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(pb, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongPtrW(pb, GWL_STYLE, style & ~(LONG_PTR)PBS_MARQUEE);
        SendMessageW(pb, PBM_SETRANGE32, 0, 1000);
        SendMessageW(pb, PBM_SETPOS, 0, 0);
    }
    InvalidateRect(pb, nullptr, TRUE);
}

void read_options_from_ui()
{
    const int sel = (int)SendMessageW(ctl(IDC_TIME_COMBO), CB_GETCURSEL, 0, 0);
    g.opt.search_seconds = kTimeChoices[(sel >= 0 && sel < (int)(sizeof(kTimeChoices) / sizeof(kTimeChoices[0]))) ? sel : 2];
    g.opt.press_buttons = SendMessageW(ctl(IDC_PRESS), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void start_search()
{
    if (!g.rom || g.ripping || g.importing)
        return;
    stop_playback();
    read_options_from_ui();
    if (g.auto_mode)
        g.opt.search_seconds = g.auto_seconds;
    g.ripper->set_options(g.opt);
    if (g.rip_thread.joinable())
        g.rip_thread.join();
    g.ripping = true;
    g.stopping = false;
    if (g.opt.search_seconds > 0) {
        progress_marquee(false);
    } else {
        progress_marquee(true);
    }
    update_buttons();
    add_log(fmt(T(S_LOG_START), g.rom_path.filename().wstring().c_str()));
    HWND wnd = g.wnd;
    Ripper* ripper = g.ripper.get();
    g.rip_thread = std::thread([wnd, ripper] {
        ripper->run();
        PostMessageW(wnd, WM_APP_RIP_DONE, 0, 0);
    });
}

void stop_search()
{
    if (!g.ripping)
        return;
    g.stopping = true;
    g.ripper->cancel();
    set_status(T(S_STATUS_FINISHING));
    update_buttons();
}

void on_progress(const RipProgress& p)
{
    if (!g.ripping)
        return;
    if (p.budget_seconds > 0) {
        const int pos = (int)std::min(1000.0, p.elapsed_seconds * 1000.0 / p.budget_seconds);
        SendMessageW(ctl(IDC_PROGRESS), PBM_SETPOS, pos, 0);
    }
    if (g.stopping || (!p.exploring && !p.finished)) {
        set_status(T(S_STATUS_FINISHING));
    } else if (p.budget_seconds > 0) {
        set_status(fmt(T(S_STATUS_RUNNING), time_text(std::min(p.elapsed_seconds, p.budget_seconds)).c_str(), time_text(p.budget_seconds).c_str(),
                       p.songs, p.candidates, p.duplicates, p.rejected, p.pending));
    } else {
        set_status(fmt(T(S_STATUS_RUNNING_FOREVER), time_text(p.elapsed_seconds).c_str(), p.songs, p.candidates, p.duplicates, p.rejected, p.pending));
    }
}

// ------------------------------------------------------------------ save
bool pick_folder(std::wstring& out, const std::wstring& start)
{
    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(T(S_SAVE_PICK_FOLDER));
        if (!start.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(g.wnd))) {
            IShellItem* res = nullptr;
            if (SUCCEEDED(dlg->GetResult(&res))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    out = p;
                    CoTaskMemFree(p);
                    ok = true;
                }
                res->Release();
            }
        }
        dlg->Release();
    }
    return ok;
}

bool save_set_to(const Path& dir, bool quiet)
{
    const std::vector<int> ids = list_checked_ids();
    if (ids.empty()) {
        error_box(T(S_ERR_NO_SONGS));
        return false;
    }
    SetTags tags;
    tags.game = narrow(get_text(IDC_GAME_EDIT));
    tags.artist = narrow(get_text(IDC_ARTIST_EDIT));

    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    WriteReport rep;
    std::string err;
    const bool ok = g.ripper->write_set(dir, tags, ids, g.full_rom, rep, err);
    SetCursor(old);
    if (!ok) {
        error_box(fmt(T(S_ERR_SAVE), widen(err).c_str()));
        return false;
    }
    add_log(fmt(L"%ls (%d)", dir.wstring().c_str(), (int)rep.files.size()));
    if (quiet)
        return true;
    const std::wstring msg = fmt(T(S_SAVE_DONE), (int)rep.files.size(), dir.wstring().c_str(), (double)rep.total_bytes / 1048576.0,
                                 (double)g.rom->info().size / 1048576.0, (double)rep.rom_bytes / 1048576.0);
    if (MessageBoxW(g.wnd, msg.c_str(), T(S_APP_TITLE), MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(g.wnd, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

void save_set()
{
    if (!g.ripper || g.ripping || g.importing)
        return;
    if (list_checked_ids().empty()) {
        error_box(T(S_ERR_NO_SONGS));
        return;
    }
    std::wstring folder;
    if (!pick_folder(folder, g.rom_path.parent_path().wstring()))
        return;
    // the set goes into its own folder named after the game
    std::string game = narrow(get_text(IDC_GAME_EDIT));
    if (game.empty())
        game = "USF";
    const Path dir = Path(folder) / path_from_utf8(sanitize_filename(game));
    save_set_to(dir, false);
}

// ----------------------------------------------------------- save states
void import_states(const std::vector<std::wstring>& files)
{
    if (!g.rom || g.ripping || g.importing || files.empty())
        return;
    stop_playback();
    if (g.task_thread.joinable())
        g.task_thread.join();
    g.importing = true;
    read_options_from_ui();
    g.ripper->set_options(g.opt);
    update_buttons();
    progress_marquee(true);

    HWND wnd = g.wnd;
    Ripper* ripper = g.ripper.get();
    g.task_thread = std::thread([wnd, ripper, files] {
        int added = 0, failed = 0;
        for (const std::wstring& f : files) {
            const Path p(f);
            PostMessageW(wnd, WM_APP_LOG, 1, (LPARAM) new std::wstring(fmt(T(S_STATUS_IMPORTING), p.filename().wstring().c_str())));
            Bytes state;
            std::string err, note;
            bool ok = load_pj64_state(p, state, err, &note);
            if (ok && !note.empty())
                PostMessageW(wnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(fmt(T(S_LOG_STATE_CONVERTED), p.filename().wstring().c_str())));
            if (ok)
                ok = ripper->add_from_state(state, state_title_from_path(p), nullptr, err);
            if (ok) {
                ++added;
            } else {
                ++failed;
                PostMessageW(wnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(fmt(T(S_ERR_STATE), p.filename().wstring().c_str(), widen(err).c_str())));
            }
        }
        PostMessageW(wnd, WM_APP_IMPORT_DONE, (WPARAM)added, (LPARAM)failed);
    });
}

void browse_states()
{
    std::vector<wchar_t> buf(32768, 0);
    std::wstring filter = T(S_IMPORT_FILTER);
    std::replace(filter.begin(), filter.end(), L'|', L'\0');
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.wnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn))
        return;
    // one file: full path.  several: folder, then the names.
    std::vector<std::wstring> files;
    const wchar_t* p = buf.data();
    const std::wstring first = p;
    p += first.size() + 1;
    if (*p == 0) {
        files.push_back(first);
    } else {
        while (*p) {
            const std::wstring name = p;
            files.push_back(first + L"\\" + name);
            p += name.size() + 1;
        }
    }
    import_states(files);
}

// Files handed to the program (dropped on the window or given on the command
// line): a ROM is opened, everything else is taken as a save state.
void open_files(const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> states;
    std::wstring rom;
    for (const std::wstring& path : paths) {
        if (rom.empty() && sniff_file(Path(path)) == FileKind::Rom)
            rom = path;
        else
            states.push_back(path);
    }
    if (!rom.empty() && !load_rom(rom))
        return;
    if (states.empty())
        return;
    if (!g.rom)
        error_box(T(S_ERR_NEED_ROM));
    else
        import_states(states);
}

void on_drop(HDROP drop)
{
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> paths;
    for (UINT i = 0; i < n; i++) {
        wchar_t path[MAX_PATH * 4];
        if (DragQueryFileW(drop, i, path, (UINT)(sizeof(path) / sizeof(path[0]))))
            paths.push_back(path);
    }
    DragFinish(drop);
    open_files(paths);
}

// --------------------------------------------------------------- preview
void toggle_play()
{
    if (g.player.playing()) {
        g.player.stop();
        set_status(g.ripping ? L"" : T(S_STATUS_READY));
        update_buttons();
        return;
    }
    const int row = list_selected_row();
    if (row < 0 || !g.ripper)
        return;
    const int id = list_id_at(row);
    SongInfo song;
    bool found = false;
    for (const SongInfo& s : g.ripper->songs()) {
        if (s.id == id) {
            song = s;
            found = true;
        }
    }
    if (!found)
        return;
    std::wstring err;
    if (!g.player.start(*g.ripper, song, g.wnd, WM_APP_PLAY_ENDED, err)) {
        error_box(T(S_ERR_AUDIO));
        return;
    }
    if (!g.ripping)
        set_status(fmt(T(S_STATUS_PLAYING), widen(song.title).c_str()));
    update_buttons();
}

void delete_selected()
{
    if (!g.ripper || g.ripping || g.importing)
        return;
    HWND lv = ctl(IDC_LIST);
    for (int row = ListView_GetNextItem(lv, -1, LVNI_SELECTED); row >= 0; row = ListView_GetNextItem(lv, -1, LVNI_SELECTED)) {
        const int id = list_id_at(row);
        if (g.player.current_id() == id)
            g.player.stop();
        g.ripper->remove_song(id);
        ListView_DeleteItem(lv, row);
    }
    update_buttons();
}

void toggle_all_checks()
{
    HWND lv = ctl(IDC_LIST);
    const int n = ListView_GetItemCount(lv);
    bool all = true;
    for (int i = 0; i < n; i++)
        if (!ListView_GetCheckState(lv, i))
            all = false;
    for (int i = 0; i < n; i++)
        ListView_SetCheckState(lv, i, all ? FALSE : TRUE);
}

// -------------------------------------------------------------- settings
void set_int(HWND dlg, int id, int v)
{
    SetDlgItemInt(dlg, id, (UINT)v, FALSE);
}

int get_int(HWND dlg, int id, int lo, int hi, int fallback)
{
    BOOL ok = FALSE;
    const UINT v = GetDlgItemInt(dlg, id, &ok, FALSE);
    if (!ok)
        return fallback;
    return std::max(lo, std::min(hi, (int)v));
}

INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM)
{
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowTextW(dlg, T(S_SET_TITLE));
        static const struct { int id; StrId s; } labels[] = {
            { IDC_SET_GROUP_EMU, S_SET_GROUP_EMU }, { IDC_SET_GROUP_SONG, S_SET_GROUP_SONG }, { IDC_SET_GROUP_OUT, S_SET_GROUP_OUT },
            { IDC_SET_EXPANSION, S_SET_EXPANSION }, { IDC_SET_IDLE_LABEL, S_SET_IDLE }, { IDC_SET_THREADS_LABEL, S_SET_THREADS },
            { IDC_SET_SEED_LABEL, S_SET_SEED }, { IDC_SET_LOOPS_LABEL, S_SET_LOOPS }, { IDC_SET_FADE_LABEL, S_SET_FADE },
            { IDC_SET_DEFLEN_LABEL, S_SET_DEFLEN }, { IDC_SET_MAXLEN_LABEL, S_SET_MAXLEN }, { IDC_SET_FULLROM, S_SET_FULLROM },
            { IDOK, S_BTN_OK }, { IDCANCEL, S_BTN_CANCEL },
        };
        for (const auto& l : labels)
            SetDlgItemTextW(dlg, l.id, T(l.s));
        CheckDlgButton(dlg, IDC_SET_EXPANSION, g.opt.rdram_size == 0x800000 ? BST_CHECKED : BST_UNCHECKED);
        set_int(dlg, IDC_SET_IDLE, (int)g.opt.idle_boot_seconds);
        set_int(dlg, IDC_SET_THREADS, g.opt.threads);
        set_int(dlg, IDC_SET_SEED, (int)(g.opt.seed & 0x7FFFFFFF));
        set_int(dlg, IDC_SET_LOOPS, g.opt.loop_count);
        set_int(dlg, IDC_SET_FADE, (int)g.opt.fade_seconds);
        set_int(dlg, IDC_SET_DEFLEN, (int)g.opt.default_length_seconds);
        set_int(dlg, IDC_SET_MAXLEN, (int)g.opt.max_render_seconds);
        CheckDlgButton(dlg, IDC_SET_FULLROM, g.full_rom ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            g.opt.rdram_size = IsDlgButtonChecked(dlg, IDC_SET_EXPANSION) == BST_CHECKED ? 0x800000 : 0x400000;
            g.opt.idle_boot_seconds = get_int(dlg, IDC_SET_IDLE, 10, 3600, 100);
            g.opt.threads = get_int(dlg, IDC_SET_THREADS, 0, 64, 0);
            g.opt.seed = (uint64_t)get_int(dlg, IDC_SET_SEED, 0, 0x7FFFFFFF, 0x5EED);
            g.opt.loop_count = get_int(dlg, IDC_SET_LOOPS, 1, 20, 2);
            g.opt.fade_seconds = get_int(dlg, IDC_SET_FADE, 0, 60, 10);
            g.opt.default_length_seconds = get_int(dlg, IDC_SET_DEFLEN, 10, 3600, 180);
            g.opt.max_render_seconds = get_int(dlg, IDC_SET_MAXLEN, 60, 3600, 480);
            g.full_rom = IsDlgButtonChecked(dlg, IDC_SET_FULLROM) == BST_CHECKED;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- layout
void make_fonts()
{
    if (g.font)
        DeleteObject(g.font);
    if (g.font_bold)
        DeleteObject(g.font_bold);
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(9, g.dpi, 72);
    g.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    g.font_bold = CreateFontIndirectW(&lf);
}

BOOL CALLBACK set_font_proc(HWND child, LPARAM)
{
    SendMessageW(child, WM_SETFONT, (WPARAM)g.font, TRUE);
    return TRUE;
}

void apply_fonts()
{
    EnumChildWindows(g.wnd, set_font_proc, 0);
    SendMessageW(ctl(IDC_START), WM_SETFONT, (WPARAM)g.font_bold, TRUE);
    SendMessageW(ctl(IDC_SAVE), WM_SETFONT, (WPARAM)g.font_bold, TRUE);
}

void layout()
{
    RECT rc;
    GetClientRect(g.wnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = S(10), rh = S(24), gap = S(8);
    HDWP dwp = BeginDeferWindowPos(32);
    auto place = [&dwp](int id, int x, int y, int w, int h) {
        dwp = DeferWindowPos(dwp, ctl(id), nullptr, x, y, std::max(w, 0), std::max(h, 0), SWP_NOZORDER | SWP_NOACTIVATE);
    };

    int y = m;
    // ROM line
    const int open_w = S(96);
    place(IDC_ROM_LABEL, m, y + S(4), S(40), rh);
    place(IDC_ROM_EDIT, m + S(44), y, W - 2 * m - S(44) - open_w - gap, rh);
    place(IDC_ROM_OPEN, W - m - open_w, y - S(1), open_w, rh + S(2));
    y += rh + S(4);
    place(IDC_ROM_INFO, m + S(44), y, W - 2 * m - S(44), S(18));
    y += S(18) + gap;

    // game / artist
    const int lab = S(72);
    const int half = (W - 2 * m - gap) / 2;
    place(IDC_GAME_LABEL, m, y + S(4), lab, rh);
    place(IDC_GAME_EDIT, m + lab, y, half - lab, rh);
    place(IDC_ARTIST_LABEL, m + half + gap, y + S(4), lab, rh);
    place(IDC_ARTIST_EDIT, m + half + gap + lab, y, half - lab, rh);
    y += rh + gap;

    // search options
    const int settings_w = S(120);
    place(IDC_TIME_LABEL, m, y + S(4), lab, rh);
    place(IDC_TIME_COMBO, m + lab, y, S(140), rh * 8);
    place(IDC_PRESS, m + lab + S(140) + S(16), y + S(2), W - (m + lab + S(140) + S(16)) - settings_w - m - gap, rh - S(2));
    place(IDC_SETTINGS, W - m - settings_w, y - S(1), settings_w, rh + S(2));
    y += rh + gap + S(2);

    // start / stop / progress
    const int start_w = S(120), stop_w = S(90), bh = S(30);
    place(IDC_START, m, y, start_w, bh);
    place(IDC_STOP, m + start_w + gap, y, stop_w, bh);
    place(IDC_PROGRESS, m + start_w + stop_w + 2 * gap, y + S(6), W - (m + start_w + stop_w + 2 * gap) - m, bh - S(12));
    y += bh + S(6);
    place(IDC_STATUS, m, y, W - 2 * m, S(18));
    y += S(18) + S(6);

    // bottom up: log, button row
    const int log_h = S(84);
    const int btn_h = S(28);
    const int log_y = H - m - log_h;
    const int btn_y = log_y - gap - btn_h;
    place(IDC_LIST, m, y, W - 2 * m, btn_y - gap - y);

    int x = m;
    const struct { int id; int w; } left[] = {
        { IDC_PLAY, 100 }, { IDC_RENAME, 112 }, { IDC_DELETE, 80 }, { IDC_SELALL, 132 }, { IDC_IMPORT, 196 },
    };
    for (const auto& b : left) {
        place(b.id, x, btn_y, S(b.w), btn_h);
        x += S(b.w) + S(6);
    }
    const int save_w = S(140), help_w = S(84);
    place(IDC_HELPBTN, W - m - help_w, btn_y, help_w, btn_h);
    place(IDC_SAVE, W - m - help_w - S(6) - save_w, btn_y, save_w, btn_h);
    place(IDC_LOG, m, log_y, W - 2 * m, log_h);
    EndDeferWindowPos(dwp);

    // let the last column take whatever width is left
    HWND lv = ctl(IDC_LIST);
    int used = 0;
    for (int i = 0; i < 6; i++)
        used += ListView_GetColumnWidth(lv, i);
    const int room = (W - 2 * m) - used - GetSystemMetrics(SM_CXVSCROLL) - S(6);
    ListView_SetColumnWidth(lv, 6, std::max(room, S(120)));
}

void set_list_columns()
{
    HWND lv = ctl(IDC_LIST);
    while (ListView_DeleteColumn(lv, 0)) {
    }
    const struct { StrId s; int w; int f; } cols[] = {
        { S_COL_TITLE, 240, LVCFMT_LEFT }, { S_COL_LENGTH, 84, LVCFMT_RIGHT }, { S_COL_KIND, 78, LVCFMT_LEFT },
        { S_COL_LOOP, 122, LVCFMT_RIGHT }, { S_COL_FOUND, 196, LVCFMT_LEFT }, { S_COL_RATE, 78, LVCFMT_RIGHT },
        { S_COL_NOTE, 160, LVCFMT_LEFT },
    };
    int i = 0;
    for (const auto& c : cols) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = const_cast<wchar_t*>(T(c.s));
        col.cx = S(c.w);
        col.fmt = c.f;
        ListView_InsertColumn(lv, i++, &col);
    }
}

HWND make(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int id)
{
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g.wnd, (HMENU)(INT_PTR)id, g.inst, nullptr);
}

void create_controls()
{
    make(L"STATIC", T(S_ROM_LABEL), 0, 0, IDC_ROM_LABEL);
    make(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_ROM_EDIT);
    make(L"BUTTON", T(S_ROM_OPEN), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_ROM_OPEN);
    make(L"STATIC", T(S_ROM_HINT), SS_NOPREFIX | SS_ENDELLIPSIS, 0, IDC_ROM_INFO);

    make(L"STATIC", T(S_GAME_LABEL), 0, 0, IDC_GAME_LABEL);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_GAME_EDIT);
    make(L"STATIC", T(S_ARTIST_LABEL), 0, 0, IDC_ARTIST_LABEL);
    make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_ARTIST_EDIT);

    make(L"STATIC", T(S_TIME_LABEL), 0, 0, IDC_TIME_LABEL);
    HWND combo = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, IDC_TIME_COMBO);
    for (StrId s : kTimeLabels)
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)T(s));
    SendMessageW(combo, CB_SETCURSEL, 2, 0);
    HWND press = make(L"BUTTON", T(S_PRESS_BUTTONS), BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_PRESS);
    SendMessageW(press, BM_SETCHECK, BST_CHECKED, 0);
    make(L"BUTTON", T(S_SETTINGS), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_SETTINGS);

    make(L"BUTTON", T(S_START), BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDC_START);
    make(L"BUTTON", T(S_STOP), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_STOP);
    make(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, IDC_PROGRESS);
    make(L"STATIC", T(S_STATUS_NO_ROM), SS_NOPREFIX | SS_ENDELLIPSIS, 0, IDC_STATUS);

    HWND lv = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_LIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    set_list_columns();

    make(L"BUTTON", T(S_PLAY), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_PLAY);
    make(L"BUTTON", T(S_RENAME), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_RENAME);
    make(L"BUTTON", T(S_DELETE), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_DELETE);
    make(L"BUTTON", T(S_SELECT_ALL), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_SELALL);
    make(L"BUTTON", T(S_IMPORT), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_IMPORT);
    make(L"BUTTON", T(S_SAVE), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_SAVE);
    make(L"BUTTON", T(S_HELP), BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_HELPBTN);

    make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, WS_EX_CLIENTEDGE, IDC_LOG);
}

int window_dpi(HWND wnd)
{
    // GetDpiForWindow exists from Windows 10 1607 on
    typedef UINT(WINAPI * Fn)(HWND);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) {
        const UINT d = fn(wnd);
        if (d)
            return (int)d;
    }
    HDC dc = GetDC(wnd);
    const int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    return d ? d : 96;
}

// ------------------------------------------------------------ window proc
void finish_auto(bool ok)
{
    g.exit_code = ok ? 0 : 1;
    DestroyWindow(g.wnd);
}

LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g.wnd = wnd;
        g.dpi = window_dpi(wnd);
        g.listener.wnd = wnd;
        make_fonts();
        create_controls();
        apply_fonts();
        DragAcceptFiles(wnd, TRUE);
        update_buttons();
        return 0;
    }
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(900);
        mm->ptMinTrackSize.y = S(560);
        return 0;
    }
    case 0x02E0 /* WM_DPICHANGED */: {
        g.dpi = HIWORD(wp);
        const RECT* r = (const RECT*)lp;
        make_fonts();
        apply_fonts();
        set_list_columns();
        SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_DROPFILES:
        on_drop((HDROP)wp);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ROM_OPEN: browse_rom(); break;
        case IDC_SETTINGS:
            DialogBoxParamW(g.inst, MAKEINTRESOURCEW(IDD_SETTINGS), wnd, settings_proc, 0);
            break;
        case IDC_START: start_search(); break;
        case IDC_STOP: stop_search(); break;
        case IDC_PLAY: toggle_play(); break;
        case IDC_RENAME: {
            const int row = list_selected_row();
            if (row >= 0) {
                SetFocus(ctl(IDC_LIST));
                ListView_EditLabel(ctl(IDC_LIST), row);
            }
            break;
        }
        case IDC_DELETE: delete_selected(); break;
        case IDC_SELALL: toggle_all_checks(); break;
        case IDC_IMPORT: browse_states(); break;
        case IDC_SAVE: save_set(); break;
        case IDC_HELPBTN:
            MessageBoxW(wnd, T(S_HELP_TEXT), T(S_APP_TITLE), MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;

    case WM_NOTIFY: {
        const NMHDR* hdr = (const NMHDR*)lp;
        if (hdr->idFrom != IDC_LIST)
            break;
        switch (hdr->code) {
        case LVN_ITEMCHANGED:
            update_buttons();
            break;
        case NM_DBLCLK:
            if (!g.player.playing())
                toggle_play();
            break;
        case LVN_KEYDOWN: {
            const NMLVKEYDOWN* kd = (const NMLVKEYDOWN*)lp;
            if (kd->wVKey == VK_F2) {
                const int row = list_selected_row();
                if (row >= 0)
                    ListView_EditLabel(ctl(IDC_LIST), row);
            } else if (kd->wVKey == VK_DELETE) {
                delete_selected();
            }
            break;
        }
        case LVN_ENDLABELEDITW: {
            const NMLVDISPINFOW* di = (const NMLVDISPINFOW*)lp;
            if (di->item.pszText && di->item.pszText[0] && g.ripper) {
                g.ripper->rename_song(list_id_at(di->item.iItem), narrow(di->item.pszText));
                return TRUE; // accept the new text
            }
            return FALSE;
        }
        }
        break;
    }

    case WM_APP_LOG: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        if (wp == 1)
            set_status(*s);
        add_log(*s);
        return 0;
    }
    case WM_APP_PROGRESS: {
        std::unique_ptr<RipProgress> p((RipProgress*)lp);
        on_progress(*p);
        return 0;
    }
    case WM_APP_SONG_ADDED: {
        std::unique_ptr<SongInfo> s((SongInfo*)lp);
        list_add(*s);
        add_log(fmt(T(S_LOG_SONG), widen(s->title).c_str(), time_text(s->length_seconds).c_str(),
                    T(s->kind == SongKind::Looping ? S_KIND_LOOP : s->kind == SongKind::Ending ? S_KIND_END : S_KIND_UNKNOWN)));
        update_buttons();
        return 0;
    }
    case WM_APP_SONG_REMOVED: {
        const int row = list_find((int)wp);
        if (row >= 0) {
            if (g.player.current_id() == (int)wp)
                g.player.stop();
            wchar_t title[256] = L"";
            ListView_GetItemText(ctl(IDC_LIST), row, 0, title, 256);
            add_log(fmt(T(S_LOG_REPLACED), title));
            ListView_DeleteItem(ctl(IDC_LIST), row);
        }
        update_buttons();
        return 0;
    }
    case WM_APP_RIP_DONE: {
        if (g.rip_thread.joinable())
            g.rip_thread.join();
        const bool was_stopped = g.stopping;
        g.ripping = false;
        g.stopping = false;
        progress_marquee(false);
        SendMessageW(ctl(IDC_PROGRESS), PBM_SETPOS, was_stopped ? 0 : 1000, 0);
        const int n = ListView_GetItemCount(ctl(IDC_LIST));
        set_status(fmt(T(was_stopped ? S_STATUS_STOPPED : S_STATUS_DONE), n));
        update_buttons();
        if (g.auto_mode) {
            const bool ok = n > 0 && save_set_to(Path(g.auto_out), true);
            finish_auto(ok);
        } else {
            FLASHWINFO fi = { sizeof(fi), wnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0 };
            FlashWindowEx(&fi);
        }
        return 0;
    }
    case WM_APP_IMPORT_DONE: {
        if (g.task_thread.joinable())
            g.task_thread.join();
        g.importing = false;
        progress_marquee(false);
        set_status(fmt(T(S_IMPORT_DONE), (int)wp, (int)lp));
        update_buttons();
        return 0;
    }
    case WM_APP_PLAY_ENDED:
        g.player.stop();
        if (lp == 1)
            error_box(T(S_ERR_AUDIO));
        if (!g.ripping && !g.importing)
            set_status(T(S_STATUS_READY));
        update_buttons();
        return 0;
    case WM_APP_AUTO:
        if (!load_rom(g.auto_rom)) {
            finish_auto(false);
            return 0;
        }
        start_search();
        return 0;

    case WM_CLOSE:
        if (g.ripping && !g.auto_mode) {
            if (MessageBoxW(wnd, T(S_CLOSE_WHILE_RUNNING), T(S_APP_TITLE), MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
        }
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        g.player.stop();
        if (g.ripper)
            g.ripper->cancel();
        if (g.rip_thread.joinable())
            g.rip_thread.join();
        if (g.task_thread.joinable())
            g.task_thread.join();
        PostQuitMessage(g.exit_code);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    g.inst = inst;

    // language: Japanese on a Japanese system, English elsewhere; /lang:xx overrides
    int japanese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_JAPANESE ? 1 : 0;
    std::vector<std::wstring> open_paths;
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        for (int i = 1; argv && i < argc; i++) {
            const std::wstring a = argv[i];
            if (a == L"/verbose" || a == L"--verbose") g.listener.verbose = true;
            else if (a == L"/lang:ja" || a == L"--lang=ja") japanese = 1;
            else if (a == L"/lang:en" || a == L"--lang=en") japanese = 0;
            else if ((a == L"/auto" || a == L"--auto") && i + 3 < argc) {
                g.auto_mode = true;
                g.auto_rom = argv[i + 1];
                g.auto_out = argv[i + 2];
                g.auto_seconds = _wtof(argv[i + 3]);
                i += 3;
            } else if (!a.empty() && a[0] != L'/')
                open_paths.push_back(a);
        }
        if (argv)
            LocalFree(argv);
    }
    set_language(japanese);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"USFMakerMainWindow";
    RegisterClassExW(&wc);

    HWND wnd = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_ACCEPTFILES, wc.lpszClassName, T(S_APP_TITLE), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700, nullptr, nullptr, inst, nullptr);
    if (!wnd)
        return 1;
    // size in real pixels now that the DPI of the monitor is known
    SetWindowPos(wnd, nullptr, 0, 0, S(1040), S(720), SWP_NOMOVE | SWP_NOZORDER);
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    if (g.auto_mode)
        PostMessageW(wnd, WM_APP_AUTO, 0, 0);
    else if (!open_paths.empty())
        open_files(open_paths);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(wnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
