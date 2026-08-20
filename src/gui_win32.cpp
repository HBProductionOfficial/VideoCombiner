// Windows front end. Plain Win32 so the result stays a single executable with
// no runtime dependencies. All the real work lives in engine.cpp, which the
// console build uses too.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "engine.hpp"
#include "plan.hpp"
#include "util.hpp"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")

// Without this the controls render in the pre-XP style.
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

enum : int {
    CTL_INPUT = 1001, CTL_INPUT_BROWSE, CTL_OUTPUT, CTL_OUTPUT_BROWSE,
    CTL_LIST, CTL_SELECT_ALL, CTL_SELECT_NONE,
    CTL_PERVIDEO, CTL_LIMIT, CTL_SIZE, CTL_FIT, CTL_MANDATORY,
    CTL_SHUFFLE, CTL_SEED, CTL_OVERWRITE,
    CTL_STATUS, CTL_PROGRESS, CTL_LOG,
    CTL_PREVIEW, CTL_START, CTL_STOP, CTL_OPEN_OUTPUT,
    // Spreadsheet export
    CTL_SHEET_ON, CTL_SHEET_FILE, CTL_SHEET_FILE_BROWSE,
    CTL_NAMES_FILE, CTL_NAMES_BROWSE,
    CTL_TITLES_FILE, CTL_TITLES_BROWSE,
    CTL_DESCS_FILE, CTL_DESCS_BROWSE,
    CTL_SHEET_TAGS, CTL_SHEET_PLAYLIST,
    CTL_SCHED_START, CTL_SCHED_EVERY, CTL_SHEET_PRIVACY
};

/// Controls that only make sense when the export is switched on.
const int kSheetControls[] = {
    CTL_SHEET_FILE, CTL_SHEET_FILE_BROWSE, CTL_NAMES_FILE, CTL_NAMES_BROWSE,
    CTL_TITLES_FILE, CTL_TITLES_BROWSE, CTL_DESCS_FILE, CTL_DESCS_BROWSE,
    CTL_SHEET_TAGS, CTL_SHEET_PLAYLIST, CTL_SCHED_START, CTL_SCHED_EVERY,
    CTL_SHEET_PRIVACY
};

enum : UINT {
    WM_VC_LOG = WM_APP + 1,
    WM_VC_PROGRESS,
    WM_VC_DONE
};

struct ProgressMessage {
    std::wstring stage;
    long long done;
    long long total;
};

// ------------------------------------------------------------------ text --

std::wstring widen(const std::string& text) {
    if (text.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring controlText(HWND parent, int id) {
    HWND control = GetDlgItem(parent, id);
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

// --------------------------------------------------------------- browsing --

bool pickFolder(HWND owner, std::wstring& chosen) {
    bool picked = false;
    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    chosen = path;
                    picked = true;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    return picked;
}

/// One file, either to read or to write. `filter` is the usual double null
/// terminated pair list.
bool pickFile(HWND owner, bool saving, const wchar_t* filter,
              const wchar_t* defaultExtension, std::wstring& chosen) {
    wchar_t buffer[MAX_PATH] = {};
    if (!chosen.empty() && chosen.size() < MAX_PATH) {
        wcscpy_s(buffer, chosen.c_str());
    }

    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = defaultExtension;
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR |
                   (saving ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);

    const BOOL ok = saving ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (!ok) return false;
    chosen = buffer;
    return true;
}

// ----------------------------------------------------------------- window --

struct SizeChoice { const wchar_t* label; const char* value; };

const SizeChoice kSizeChoices[] = {
    {L"Same as clips",              "source"},
    {L"TikTok / Shorts  1080x1920", "vertical"},
    {L"YouTube  1920x1080",         "horizontal"},
    {L"Square  1080x1080",          "square"},
    {L"Portrait  1080x1350",        "portrait"},
    {L"720p  1280x720",             "720p"},
    {L"1080p  1920x1080",           "1080p"},
    {L"1440p  2560x1440",           "1440p"},
    {L"4K  3840x2160",              "4k"},
    {L"Vertical small  720x1280",   "vertical720"},
};

const wchar_t* kFitChoices[] = {
    L"Fit inside, add bars",
    L"Fill frame, crop edges",
    L"Fill with blurred background",
    L"Stretch to fit",
};

class App {
public:
    HWND window = nullptr;
    std::vector<vc::fs::path> clips;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRequested{false};
    std::thread worker;
    HFONT font = nullptr;

    ~App() {
        if (worker.joinable()) worker.join();
        if (font) DeleteObject(font);
    }

    /// Reads every control into a Config. One place so preview and start can
    /// never disagree about what the window says.
    vc::Config readConfig() const {
        vc::Config cfg;
        cfg.input = narrow(controlText(window, CTL_INPUT));
        cfg.output = narrow(controlText(window, CTL_OUTPUT));

        // Only the ticked clips.
        HWND list = GetDlgItem(window, CTL_LIST);
        int count = ListView_GetItemCount(list);
        for (int i = 0; i < count; ++i) {
            if (ListView_GetCheckState(list, i)) {
                cfg.clips.push_back(clips[static_cast<size_t>(i)].string());
            }
        }

        cfg.clipsPerVideo = _wtoi(controlText(window, CTL_PERVIDEO).c_str());
        if (cfg.clipsPerVideo < 1) cfg.clipsPerVideo = 3;

        cfg.limit = _wtoi64(controlText(window, CTL_LIMIT).c_str());
        if (cfg.limit < 0) cfg.limit = 0;

        std::wstring sizeText = controlText(window, CTL_SIZE);
        std::string sizeValue = narrow(sizeText);
        for (const SizeChoice& choice : kSizeChoices) {
            if (sizeText == choice.label) { sizeValue = choice.value; break; }
        }
        vc::resolveSize(sizeValue, cfg.width, cfg.height);

        switch ((int)SendDlgItemMessageW(window, CTL_FIT, CB_GETCURSEL, 0, 0)) {
            case 1: cfg.fit = vc::Config::Fit::Cover; break;
            case 2: cfg.fit = vc::Config::Fit::Blur; break;
            case 3: cfg.fit = vc::Config::Fit::Stretch; break;
            default: cfg.fit = vc::Config::Fit::Contain; break;
        }

        int mandatory = (int)SendDlgItemMessageW(window, CTL_MANDATORY, CB_GETCURSEL, 0, 0);
        if (mandatory > 0) {
            wchar_t buffer[MAX_PATH] = {};
            SendDlgItemMessageW(window, CTL_MANDATORY, CB_GETLBTEXT, mandatory, (LPARAM)buffer);
            cfg.mandatory.push_back(narrow(buffer));
        }

        cfg.shuffle = IsDlgButtonChecked(window, CTL_SHUFFLE) == BST_CHECKED;
        cfg.overwrite = IsDlgButtonChecked(window, CTL_OVERWRITE) == BST_CHECKED;
        cfg.seed = (unsigned)_wtoi(controlText(window, CTL_SEED).c_str());

        if (IsDlgButtonChecked(window, CTL_SHEET_ON) == BST_CHECKED) {
            cfg.exportPath = narrow(controlText(window, CTL_SHEET_FILE));
            cfg.namesFile = narrow(controlText(window, CTL_NAMES_FILE));
            cfg.titleVariantsFile = narrow(controlText(window, CTL_TITLES_FILE));
            cfg.descriptionVariantsFile = narrow(controlText(window, CTL_DESCS_FILE));
            cfg.sheetTags = narrow(controlText(window, CTL_SHEET_TAGS));
            cfg.sheetPlaylist = narrow(controlText(window, CTL_SHEET_PLAYLIST));
            cfg.scheduleStart = narrow(controlText(window, CTL_SCHED_START));

            const long long every =
                vc::parseDuration(narrow(controlText(window, CTL_SCHED_EVERY)));
            cfg.scheduleEvery = every > 0 ? every : 0;

            const int privacy =
                (int)SendDlgItemMessageW(window, CTL_SHEET_PRIVACY, CB_GETCURSEL, 0, 0);
            if (privacy >= 0) {
                wchar_t buffer[32] = {};
                SendDlgItemMessageW(window, CTL_SHEET_PRIVACY, CB_GETLBTEXT, privacy,
                                    (LPARAM)buffer);
                cfg.sheetPrivacy = narrow(buffer);
            }
        }
        return cfg;
    }

    /// The export fields mean nothing unless the box is ticked, so they follow
    /// it rather than sitting there looking editable.
    void updateSheetEnabled() {
        const bool on = IsDlgButtonChecked(window, CTL_SHEET_ON) == BST_CHECKED;
        for (int id : kSheetControls) {
            EnableWindow(GetDlgItem(window, id), on && !running);
        }
    }

    void setStatus(const std::wstring& text) {
        SetDlgItemTextW(window, CTL_STATUS, text.c_str());
    }

    void appendLog(const std::wstring& line) {
        HWND log = GetDlgItem(window, CTL_LOG);
        int length = GetWindowTextLengthW(log);
        SendMessageW(log, EM_SETSEL, length, length);
        std::wstring text = line + L"\r\n";
        SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
        SendMessageW(log, WM_VSCROLL, SB_BOTTOM, 0);
    }

    void clearLog() { SetDlgItemTextW(window, CTL_LOG, L""); }

    /// Rescans the clips folder and refills the list and the mandatory picker.
    void refreshClips() {
        vc::Config cfg;
        cfg.input = narrow(controlText(window, CTL_INPUT));
        std::string problem;
        clips = vc::selectClips(cfg, problem);

        HWND list = GetDlgItem(window, CTL_LIST);
        ListView_DeleteAllItems(list);
        SendDlgItemMessageW(window, CTL_MANDATORY, CB_RESETCONTENT, 0, 0);
        SendDlgItemMessageW(window, CTL_MANDATORY, CB_ADDSTRING, 0, (LPARAM)L"(none)");

        for (size_t i = 0; i < clips.size(); ++i) {
            std::wstring name = widen(clips[i].filename().string());
            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = (int)i;
            item.pszText = name.data();
            ListView_InsertItem(list, &item);
            ListView_SetCheckState(list, (int)i, TRUE);
            SendDlgItemMessageW(window, CTL_MANDATORY, CB_ADDSTRING, 0, (LPARAM)name.c_str());
        }
        SendDlgItemMessageW(window, CTL_MANDATORY, CB_SETCURSEL, 0, 0);

        if (!problem.empty()) setStatus(widen(problem));
        updateCounts();
    }

    /// Works out how many videos the current settings describe. No ffmpeg, so
    /// this is cheap enough to run on every change.
    void updateCounts() {
        if (running) return;
        vc::Config cfg = readConfig();
        if (cfg.clips.empty()) {
            setStatus(L"No clips ticked.");
            return;
        }
        vc::Preview p = vc::preview(cfg);
        if (!p.problem.empty()) {
            setStatus(widen(p.problem));
            return;
        }
        std::wstring text = std::to_wstring(cfg.clips.size()) + L" clips ticked, ";
        if (p.possible < 0) {
            text += L"more videos possible than can be counted";
        } else {
            text += std::to_wstring(p.possible) + L" possible";
            long long building = (cfg.limit > 0 && cfg.limit < p.possible) ? cfg.limit : p.possible;
            text += L", building " + std::to_wstring(building);
        }
        setStatus(text);
    }

    void setBusy(bool busy) {
        running = busy;
        for (int id : {CTL_INPUT_BROWSE, CTL_OUTPUT_BROWSE, CTL_START, CTL_PREVIEW,
                       CTL_SELECT_ALL, CTL_SELECT_NONE, CTL_LIST, CTL_PERVIDEO,
                       CTL_LIMIT, CTL_SIZE, CTL_FIT, CTL_MANDATORY, CTL_SHUFFLE,
                       CTL_SEED, CTL_OVERWRITE, CTL_INPUT, CTL_OUTPUT, CTL_SHEET_ON}) {
            EnableWindow(GetDlgItem(window, id), !busy);
        }
        EnableWindow(GetDlgItem(window, CTL_STOP), busy);
        updateSheetEnabled();
    }

    void startRun(bool dryRun) {
        if (running) return;
        if (worker.joinable()) worker.join();

        vc::Config cfg = readConfig();
        if (cfg.clips.empty()) {
            MessageBoxW(window, L"Tick at least one clip first.", L"VideoCombiner",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        cfg.dryRun = dryRun;

        clearLog();
        stopRequested = false;
        setBusy(true);
        SendDlgItemMessageW(window, CTL_PROGRESS, PBM_SETPOS, 0, 0);

        HWND target = window;
        std::atomic<bool>* cancelFlag = &stopRequested;

        worker = std::thread([cfg, target, cancelFlag]() {
            vc::Callbacks callbacks;
            callbacks.log = [target](const std::string& line) {
                PostMessageW(target, WM_VC_LOG, 0, (LPARAM)new std::wstring(widen(line)));
            };
            callbacks.progress = [target](const std::string& stage, long long done, long long total) {
                auto* message = new ProgressMessage{widen(stage), done, total};
                PostMessageW(target, WM_VC_PROGRESS, 0, (LPARAM)message);
            };
            callbacks.cancelled = [cancelFlag]() { return cancelFlag->load(); };

            vc::RunStats stats;
            try {
                stats = vc::run(cfg, callbacks);
            } catch (const std::exception& e) {
                stats.ok = false;
                stats.error = e.what();
                PostMessageW(target, WM_VC_LOG, 0,
                             (LPARAM)new std::wstring(widen(std::string("error: ") + e.what())));
            }
            PostMessageW(target, WM_VC_DONE, 0, (LPARAM)new vc::RunStats(stats));
        });
    }

    void requestStop() { stopRequested = true; setStatus(L"Stopping after the current video..."); }

    void openOutputFolder() {
        std::wstring folder = controlText(window, CTL_OUTPUT);
        if (folder.empty()) return;
        ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
};

App* appOf(HWND window) {
    return reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

HWND addControl(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
                int x, int y, int width, int height, int id, HFONT font) {
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, parent,
                                   (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
    return control;
}

void buildLayout(HWND window, App& app) {
    // The shell's message font, so this looks like the rest of Windows.
    NONCLIENTMETRICSW metrics = {sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    app.font = CreateFontIndirectW(&metrics.lfMessageFont);
    HFONT font = app.font;

    const int margin = 12;
    const int labelWidth = 92;
    const int rowHeight = 24;
    const int fullWidth = 700 - margin * 2;
    const int right = margin + fullWidth;
    const int col2 = margin + 330;
    int y = margin;

    addControl(window, L"STATIC", L"Clips folder", SS_LEFT, margin, y + 4, labelWidth, 18, -1, font);
    addControl(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
               margin + labelWidth, y, fullWidth - labelWidth - 90, 22, CTL_INPUT, font);
    addControl(window, L"BUTTON", L"Browse", BS_PUSHBUTTON, right - 84, y - 1, 84, 24,
               CTL_INPUT_BROWSE, font);
    y += rowHeight + 6;

    addControl(window, L"STATIC", L"Output folder", SS_LEFT, margin, y + 4, labelWidth, 18, -1, font);
    addControl(window, L"EDIT", L"output", WS_BORDER | ES_AUTOHSCROLL,
               margin + labelWidth, y, fullWidth - labelWidth - 90, 22, CTL_OUTPUT, font);
    addControl(window, L"BUTTON", L"Browse", BS_PUSHBUTTON, right - 84, y - 1, 84, 24,
               CTL_OUTPUT_BROWSE, font);
    y += rowHeight + 10;

    addControl(window, L"STATIC", L"Clips to use", SS_LEFT, margin, y, 200, 18, -1, font);
    addControl(window, L"BUTTON", L"Tick all", BS_PUSHBUTTON, right - 176, y - 4, 84, 24,
               CTL_SELECT_ALL, font);
    addControl(window, L"BUTTON", L"Tick none", BS_PUSHBUTTON, right - 84, y - 4, 84, 24,
               CTL_SELECT_NONE, font);
    y += 24;

    HWND list = addControl(window, WC_LISTVIEWW, L"",
                           LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS | WS_BORDER,
                           margin, y, fullWidth, 120, CTL_LIST, font);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNW column = {};
    column.mask = LVCF_WIDTH;
    column.cx = fullWidth - 24;
    ListView_InsertColumn(list, 0, &column);
    y += 130;

    addControl(window, L"STATIC", L"Clips per video", SS_LEFT, margin, y + 4, 100, 18, -1, font);
    HWND perVideo = addControl(window, L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL,
                               margin + 104, y, 70, 200, CTL_PERVIDEO, font);
    for (int i = 2; i <= 8; ++i) {
        SendMessageW(perVideo, CB_ADDSTRING, 0, (LPARAM)std::to_wstring(i).c_str());
    }
    SetWindowTextW(perVideo, L"3");

    addControl(window, L"STATIC", L"Limit", SS_LEFT, col2, y + 4, 40, 18, -1, font);
    addControl(window, L"EDIT", L"50", WS_BORDER | ES_NUMBER, col2 + 44, y, 80, 22,
               CTL_LIMIT, font);
    addControl(window, L"STATIC", L"0 means every one", SS_LEFT, col2 + 132, y + 4, 150, 18,
               -1, font);
    y += rowHeight + 6;

    addControl(window, L"STATIC", L"Output size", SS_LEFT, margin, y + 4, 100, 18, -1, font);
    HWND sizeBox = addControl(window, L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL,
                              margin + 104, y, 200, 300, CTL_SIZE, font);
    for (const SizeChoice& choice : kSizeChoices) {
        SendMessageW(sizeBox, CB_ADDSTRING, 0, (LPARAM)choice.label);
    }
    SendMessageW(sizeBox, CB_SETCURSEL, 0, 0);

    addControl(window, L"STATIC", L"Fit", SS_LEFT, col2, y + 4, 40, 18, -1, font);
    HWND fitBox = addControl(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                             col2 + 44, y, 230, 200, CTL_FIT, font);
    for (const wchar_t* choice : kFitChoices) {
        SendMessageW(fitBox, CB_ADDSTRING, 0, (LPARAM)choice);
    }
    SendMessageW(fitBox, CB_SETCURSEL, 0, 0);
    y += rowHeight + 6;

    addControl(window, L"STATIC", L"Always include", SS_LEFT, margin, y + 4, 100, 18, -1, font);
    HWND mandatory = addControl(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                margin + 104, y, 200, 300, CTL_MANDATORY, font);
    SendMessageW(mandatory, CB_ADDSTRING, 0, (LPARAM)L"(none)");
    SendMessageW(mandatory, CB_SETCURSEL, 0, 0);

    addControl(window, L"BUTTON", L"Shuffle", BS_AUTOCHECKBOX, col2 + 44, y + 2, 74, 20,
               CTL_SHUFFLE, font);
    addControl(window, L"STATIC", L"Seed", SS_LEFT, col2 + 124, y + 4, 34, 18, -1, font);
    addControl(window, L"EDIT", L"0", WS_BORDER | ES_NUMBER, col2 + 160, y, 60, 22,
               CTL_SEED, font);
    addControl(window, L"BUTTON", L"Overwrite", BS_AUTOCHECKBOX, col2 + 228, y + 2, 90, 20,
               CTL_OVERWRITE, font);
    CheckDlgButton(window, CTL_SHUFFLE, BST_CHECKED);
    y += rowHeight + 12;

    // ------------------------------------------------- spreadsheet export --
    addControl(window, L"BUTTON",
               L"Also write a spreadsheet of these videos for an uploader",
               BS_AUTOCHECKBOX, margin, y, 420, 20, CTL_SHEET_ON, font);
    y += 26;

    const int sheetLabel = 104;
    const int sheetEditX = margin + sheetLabel + 4;
    const int sheetEditW = fullWidth - sheetLabel - 94;

    addControl(window, L"STATIC", L"Sheet file", SS_LEFT, margin, y + 4, sheetLabel, 18, -1, font);
    addControl(window, L"EDIT", L"shorts.csv", WS_BORDER | ES_AUTOHSCROLL,
               sheetEditX, y, sheetEditW, 22, CTL_SHEET_FILE, font);
    addControl(window, L"BUTTON", L"Browse", BS_PUSHBUTTON, right - 84, y - 1, 84, 24,
               CTL_SHEET_FILE_BROWSE, font);
    y += 28;

    addControl(window, L"STATIC", L"Clip names", SS_LEFT, margin, y + 4, sheetLabel, 18, -1, font);
    addControl(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
               sheetEditX, y, sheetEditW, 22, CTL_NAMES_FILE, font);
    addControl(window, L"BUTTON", L"Browse", BS_PUSHBUTTON, right - 84, y - 1, 84, 24,
               CTL_NAMES_BROWSE, font);
    y += 28;

    addControl(window, L"STATIC", L"Titles", SS_LEFT, margin, y + 4, sheetLabel, 18, -1, font);
    addControl(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
               sheetEditX, y, sheetEditW, 22, CTL_TITLES_FILE, font);
    addControl(window, L"BUTTON", L"Browse", BS_PUSHBUTTON, right - 84, y - 1, 84, 24,
               CTL_TITLES_BROWSE, font);
    y += 28;

    addControl(window, L"STATIC", L"Descriptions", SS_LEFT, margin, y + 4, sheetLabel, 18, -1, font);
    addControl(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
               sheetEditX, y, sheetEditW, 22, CTL_DESCS_FILE, font);
    addControl(window, L"BUTTON", L"Browse", BS_PUSHBUTTON, right - 84, y - 1, 84, 24,
               CTL_DESCS_BROWSE, font);
    y += 30;

    addControl(window, L"STATIC", L"Base tags", SS_LEFT, margin, y + 4, sheetLabel, 18, -1, font);
    addControl(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
               sheetEditX, y, 196, 22, CTL_SHEET_TAGS, font);
    addControl(window, L"STATIC", L"Playlist", SS_LEFT, col2 + 20, y + 4, 54, 18, -1, font);
    addControl(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
               col2 + 78, y, 268, 22, CTL_SHEET_PLAYLIST, font);
    y += 28;

    addControl(window, L"STATIC", L"First upload", SS_LEFT, margin, y + 4, sheetLabel, 18, -1, font);
    addControl(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
               sheetEditX, y, 170, 22, CTL_SCHED_START, font);
    addControl(window, L"STATIC", L"Every", SS_LEFT, margin + 294, y + 4, 44, 18, -1, font);
    addControl(window, L"EDIT", L"8h", WS_BORDER | ES_AUTOHSCROLL,
               margin + 340, y, 60, 22, CTL_SCHED_EVERY, font);
    addControl(window, L"STATIC", L"Privacy", SS_LEFT, margin + 414, y + 4, 52, 18, -1, font);
    HWND privacy = addControl(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                              margin + 470, y, 120, 200, CTL_SHEET_PRIVACY, font);
    for (const wchar_t* choice : {L"public", L"unlisted", L"private"}) {
        SendMessageW(privacy, CB_ADDSTRING, 0, (LPARAM)choice);
    }
    SendMessageW(privacy, CB_SETCURSEL, 0, 0);
    y += 32;

    addControl(window, L"STATIC", L"", SS_LEFT, margin, y, fullWidth, 18, CTL_STATUS, font);
    y += 22;

    addControl(window, PROGRESS_CLASSW, L"", 0, margin, y, fullWidth, 18, CTL_PROGRESS, font);
    y += 26;

    addControl(window, L"EDIT", L"",
               WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
               margin, y, fullWidth, 120, CTL_LOG, font);
    y += 130;

    addControl(window, L"BUTTON", L"Open output folder", BS_PUSHBUTTON, margin, y, 150, 28,
               CTL_OPEN_OUTPUT, font);
    addControl(window, L"BUTTON", L"Preview", BS_PUSHBUTTON, right - 270, y, 84, 28,
               CTL_PREVIEW, font);
    addControl(window, L"BUTTON", L"Stop", BS_PUSHBUTTON, right - 178, y, 84, 28,
               CTL_STOP, font);
    addControl(window, L"BUTTON", L"Start", BS_DEFPUSHBUTTON, right - 86, y, 86, 28,
               CTL_START, font);
    EnableWindow(GetDlgItem(window, CTL_STOP), FALSE);

    app.updateSheetEnabled();
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = appOf(window);

    switch (message) {
        case WM_CREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<App*>(create->lpCreateParams);
            app->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            buildLayout(window, *app);

            wchar_t here[MAX_PATH] = {};
            GetCurrentDirectoryW(MAX_PATH, here);
            SetDlgItemTextW(window, CTL_INPUT, here);
            SetDlgItemTextW(window, CTL_OUTPUT, (std::wstring(here) + L"\\output").c_str());
            app->refreshClips();
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);

            if (id == CTL_INPUT_BROWSE || id == CTL_OUTPUT_BROWSE) {
                std::wstring chosen;
                if (pickFolder(window, chosen)) {
                    SetDlgItemTextW(window, id == CTL_INPUT_BROWSE ? CTL_INPUT : CTL_OUTPUT,
                                    chosen.c_str());
                    if (id == CTL_INPUT_BROWSE) app->refreshClips();
                }
                return 0;
            }
            if (id == CTL_INPUT && code == EN_KILLFOCUS) { app->refreshClips(); return 0; }
            if (id == CTL_SELECT_ALL || id == CTL_SELECT_NONE) {
                HWND list = GetDlgItem(window, CTL_LIST);
                BOOL state = (id == CTL_SELECT_ALL);
                for (int i = 0; i < ListView_GetItemCount(list); ++i) {
                    ListView_SetCheckState(list, i, state);
                }
                app->updateCounts();
                return 0;
            }
            if (id == CTL_SHEET_ON) { app->updateSheetEnabled(); app->updateCounts(); return 0; }
            if (id == CTL_SHEET_FILE_BROWSE) {
                std::wstring chosen = controlText(window, CTL_SHEET_FILE);
                if (pickFile(window, true,
                             L"Spreadsheet (*.csv;*.tsv;*.json)\0*.csv;*.tsv;*.json\0"
                             L"All files\0*.*\0",
                             L"csv", chosen)) {
                    SetDlgItemTextW(window, CTL_SHEET_FILE, chosen.c_str());
                }
                return 0;
            }
            if (id == CTL_NAMES_BROWSE || id == CTL_TITLES_BROWSE || id == CTL_DESCS_BROWSE) {
                const int field = (id == CTL_NAMES_BROWSE)  ? CTL_NAMES_FILE
                                : (id == CTL_TITLES_BROWSE) ? CTL_TITLES_FILE
                                                            : CTL_DESCS_FILE;
                const wchar_t* filter = (id == CTL_NAMES_BROWSE)
                    ? L"JSON (*.json)\0*.json\0All files\0*.*\0"
                    : L"Text (*.txt)\0*.txt\0All files\0*.*\0";
                std::wstring chosen = controlText(window, field);
                if (pickFile(window, false, filter, nullptr, chosen)) {
                    SetDlgItemTextW(window, field, chosen.c_str());
                }
                return 0;
            }
            if (id == CTL_START)   { app->startRun(false); return 0; }
            if (id == CTL_PREVIEW) { app->startRun(true);  return 0; }
            if (id == CTL_STOP)    { app->requestStop();   return 0; }
            if (id == CTL_OPEN_OUTPUT) { app->openOutputFolder(); return 0; }

            // Anything that changes the plan refreshes the count.
            const bool changed =
                (code == CBN_SELCHANGE || code == CBN_EDITCHANGE || code == EN_CHANGE ||
                 code == BN_CLICKED);
            if (changed && (id == CTL_PERVIDEO || id == CTL_LIMIT || id == CTL_MANDATORY ||
                            id == CTL_SHUFFLE || id == CTL_SEED || id == CTL_SIZE ||
                            id == CTL_FIT || id == CTL_OVERWRITE)) {
                app->updateCounts();
            }
            return 0;
        }

        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom == CTL_LIST && header->code == LVN_ITEMCHANGED) {
                auto* changed = reinterpret_cast<NMLISTVIEW*>(lParam);
                // Only react to the checkbox part of the state, not selection.
                if (changed->uChanged & LVIF_STATE) {
                    if ((changed->uOldState & LVIS_STATEIMAGEMASK) !=
                        (changed->uNewState & LVIS_STATEIMAGEMASK)) {
                        app->updateCounts();
                    }
                }
            }
            return 0;
        }

        case WM_VC_LOG: {
            std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lParam));
            app->appendLog(*line);
            return 0;
        }

        case WM_VC_PROGRESS: {
            std::unique_ptr<ProgressMessage> update(reinterpret_cast<ProgressMessage*>(lParam));
            HWND bar = GetDlgItem(window, CTL_PROGRESS);
            if (update->total > 0) {
                SendMessageW(bar, PBM_SETRANGE32, 0, (LPARAM)update->total);
                SendMessageW(bar, PBM_SETPOS, (WPARAM)update->done, 0);
                app->setStatus(update->stage + L"  " + std::to_wstring(update->done) +
                               L" of " + std::to_wstring(update->total));
            }
            return 0;
        }

        case WM_VC_DONE: {
            std::unique_ptr<vc::RunStats> stats(reinterpret_cast<vc::RunStats*>(lParam));
            app->setBusy(false);
            if (stats->cancelled) {
                app->setStatus(L"Stopped. " + std::to_wstring(stats->built) + L" built.");
            } else if (!stats->ok) {
                app->setStatus(L"Finished with errors. " + widen(stats->error));
            } else if (stats->planned == 0) {
                app->setStatus(L"Nothing to do, everything is already built.");
            } else {
                app->setStatus(L"Done. " + std::to_wstring(stats->built) + L" built in " +
                               widen(vc::formatDuration(stats->seconds)) + L".");
            }
            app->updateCounts();
            return 0;
        }

        case WM_CLOSE:
            if (app && app->running) {
                if (MessageBoxW(window, L"A run is in progress. Stop it and close?",
                                L"VideoCombiner", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    return 0;
                }
                app->requestStop();
                if (app->worker.joinable()) app->worker.join();
            }
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS |
                                                       ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    App app;

    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"VideoCombinerWindow";
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&windowClass);

    RECT wanted = {0, 0, 700, 745};
    // Fixed size, so no layout code is needed when the user drags an edge.
    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX);
    AdjustWindowRect(&wanted, style, FALSE);

    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"VideoCombiner",
                                  style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  wanted.right - wanted.left, wanted.bottom - wanted.top,
                                  nullptr, nullptr, instance, &app);
    if (!window) {
        MessageBoxW(nullptr, L"Could not create the window.", L"VideoCombiner", MB_ICONERROR);
        return 1;
    }
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    CoUninitialize();
    return 0;
}
