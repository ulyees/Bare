// =============================================================================
//  Bare — 极简文本编辑器（单文件 Win32 / C++17）
//  单文件原生实现：无运行时依赖、内存映射加载、只绘制可视行
//  功能：打开 / 编辑 / 撤销重做 / 查找替换 / 自动换行 / 编码切换 / 编码与换行显示
//  编译： build-msvc.bat (MSVC) / build.bat (MinGW)
// =============================================================================
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

// -----------------------------------------------------------------------------
// 编码描述
// -----------------------------------------------------------------------------
struct Enc {
    enum Kind { UTF8 = 0, UTF16LE, UTF16BE, ANSI, BYTES } kind = UTF8;
    bool bom = false;
    size_t skip = 0;

    const wchar_t* Name() const {
        switch (kind) {
            case UTF8:    return bom ? L"UTF-8 BOM" : L"UTF-8";
            case UTF16LE: return L"UTF-16 LE";
            case UTF16BE: return L"UTF-16 BE";
            case ANSI:    return L"ANSI / GBK";
            default:      return L"字节模式";
        }
    }
};

// -----------------------------------------------------------------------------
// 查找 / 替换状态
// -----------------------------------------------------------------------------
struct FindState {
    bool open = false;
    bool replaceMode = false;                     // 是否显示替换行
    int  activeField = 0;                         // 0=查找 1=替换
    std::wstring q;                               // 查询串
    std::wstring rep;                             // 替换串
    bool caseSensitive = false;                   // 区分大小写
    bool fromStart = false;                       // 每次从文档开头查找
    std::vector<std::pair<size_t, size_t>> hits;  // (行, 起始列)
    int current = -1;
    bool dirtyHits = true;
    bool truncated = false;
};

// -----------------------------------------------------------------------------
// 撤销记录：把 [l1,c1)-(l2,c2) 的 oldText 替换成 newText
// -----------------------------------------------------------------------------
struct UndoRec {
    size_t l1 = 0, c1 = 0, l2 = 0, c2 = 0;
    std::wstring oldText, newText;
    size_t caretBeforeL = 0, caretBeforeC = 0;
    size_t caretAfterL = 0, caretAfterC = 0;
};

// -----------------------------------------------------------------------------
// 全局状态（单窗口、单文档，多窗口用多进程实现）
// -----------------------------------------------------------------------------
struct App {
    HWND hwnd = nullptr;
    HWND hRename = nullptr;                        // 工具栏内联重命名编辑框
    HINSTANCE hinst = nullptr;
    HFONT hMono = nullptr, hUI = nullptr, hUIBold = nullptr;

    std::vector<std::wstring> lines;               // 每行保留行尾 '\r'（CRLF）
    std::wstring path;
    Enc enc;
    bool dirty = false;
    bool endsWithNL = false;
    bool crlfStyle = true;
    std::wstring eolName = L"CRLF";
    unsigned long long fileSize = 0;

    // 外部修改检测
    FILETIME stamp = {};
    bool hasStamp = false;
    bool watching = false;                         // 正在弹外部修改提示

    // 光标 / 选区
    size_t curL = 0, curC = 0;
    bool hasSel = false;
    size_t anchorL = 0, anchorC = 0;
    bool selecting = false;
    int desiredX = -1;
    wchar_t pendingHigh = 0;

    // 视图
    size_t topL = 0;
    int xOffset = 0;
    int maxLinePx = 0;

    int dpi = 96, fontPx = 16, lineH = 22, charW = 9;
    int gutterW = 56, toolbarH = 44, statusH = 26, findH = 40;
    int clientW = 0, clientH = 0;

    int hoverBtn = 0, downBtn = 0;                 // 1打开 2保存 3另存为 4置顶 5换行
    bool hoverName = false;
    bool hoverFind = false;
    int hoverFindBtn = 0;                          // 查找栏：1上 2下 3Aa 4关闭 5替换 6全部 7从头
    FindState find;

    bool hasFocus = true, caretOn = true;
    bool dark = false;
    bool wrap = false;                             // 自动换行
    bool topmost = false;                          // 窗口置顶
    bool showEol = false;                          // 显示行尾符号

    std::wstring flash;                            // 状态栏短暂提示
    unsigned long long flashUntil = 0;
    unsigned long long lastEditTick = 0;

    // 撤销栈
    std::vector<UndoRec> undo, redo;
    size_t savedDepth = 0;

    // 窗口几何记忆
    RECT winRect = { 0, 0, 0, 0 };
    bool winMax = false;

    COLORREF cBg = 0, cText = 0, cGutter = 0, cGutterText = 0;
    COLORREF cBar = 0, cBarText = 0, cSep = 0, cStatusText = 0;
    COLORREF cBtn = 0, cBtnHover = 0, cBtnDown = 0, cBtnBorder = 0;
    COLORREF cSel = 0, cSelText = 0, cCurLine = 0, cCaret = 0;
    COLORREF cField = 0, cFieldBorder = 0, cFieldText = 0, cPh = 0;
    COLORREF cFind = 0, cFindText = 0, cAccent = 0;
};
static App g;

static const wchar_t* kRegPath = L"Software\\Bare";

// 前向声明
static void UpdateTitle();
static void UpdateScroll();
static void EnsureCaretVisible();
static void UpdateImePos();
static void UpdateDirty();
static bool ConfirmDiscard();
static void LoadFile(const std::wstring& p, int forceKind = -1, bool skipDiscard = false);
static void SaveFile(bool saveAs);
static void OpenDialog();
static void Refresh();
static void AfterEdit();
static void AfterMove(bool resetDesired);
static void CloseFind();
static void ResetFind();
static void SyncFindHeight();
static void Flash(const wchar_t* s);
static void LoadSettings();
static void SaveSettings();
static bool ShouldOpenNewWindow();
static void LaunchNewWindow(const std::wstring& p);

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------
static inline bool IsHigh(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
static inline bool IsLow(wchar_t c)  { return c >= 0xDC00 && c <= 0xDFFF; }

static std::wstring FileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

static std::wstring DirName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}

static std::wstring FmtSize(unsigned long long b) {
    wchar_t t[64];
    if (b < 1024ULL)              swprintf(t, 64, L"%llu B", b);
    else if (b < 1024ULL * 1024) swprintf(t, 64, L"%.1f KB", (double)b / 1024.0);
    else                         swprintf(t, 64, L"%.1f MB", (double)b / 1048576.0);
    return std::wstring(t);
}

static void Fill(HDC h, int x0, int y0, int x1, int y1, COLORREF c) {
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(h, &r, b);
    DeleteObject(b);
}

static void Line(HDC h, int x0, int y0, int x1, int y1, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ o = SelectObject(h, p);
    MoveToEx(h, x0, y0, nullptr);
    LineTo(h, x1, y1);
    SelectObject(h, o);
    DeleteObject(p);
}

static inline size_t VLen(const std::wstring& s) {
    return s.size() - ((!s.empty() && s.back() == L'\r') ? 1 : 0);
}

// 显示串：控制字符画成中点，列下标一一对应
static std::wstring Disp(const std::wstring& s) {
    std::wstring d(s.data(), VLen(s));
    for (wchar_t& c : d)
        if (c < 32 || (c >= 0x7F && c <= 0x9F)) c = 0x00B7;
    return d;
}

static void ComputeDx(HDC hdc, const std::wstring& d, std::vector<int>& dx) {
    dx.assign(d.size() + 1, 0);
    if (d.empty()) return;
    std::vector<int> ext(d.size(), 0);
    SIZE sz = { 0, 0 };
    int fit = 0;
    if (GetTextExtentExPointW(hdc, d.c_str(), (int)d.size(), 0x3FFFFFFF,
                              &fit, ext.data(), &sz)) {
        for (size_t i = 0; i < d.size(); ++i) dx[i + 1] = ext[i];
    } else {
        for (size_t i = 0; i < d.size(); ++i) {
            SIZE s2 = { 0, 0 };
            GetTextExtentPoint32W(hdc, d.c_str() + i, 1, &s2);
            dx[i + 1] = dx[i] + s2.cx;
        }
    }
}

static size_t ColFromX(const std::wstring& d, const std::vector<int>& dx, int px) {
    if (d.empty() || px <= 0) return 0;
    auto it = std::upper_bound(dx.begin() + 1, dx.end(), px);
    size_t c = (size_t)(it - dx.begin()) - 1;
    size_t n = d.size();
    if (c >= n) return n;
    if (c + 1 <= n && (px - dx[c]) > (dx[c + 1] - px)) ++c;
    return std::min(c, n);
}

// -----------------------------------------------------------------------------
// 编码探测 / 解码
// -----------------------------------------------------------------------------
static bool IsValidUTF8(const BYTE* p, size_t n) {
    for (size_t i = 0; i < n;) {
        BYTE c = p[i];
        if (c < 0x80) { ++i; continue; }
        int need; unsigned int cp;
        if      (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; }
        else return false;
        if (i + (size_t)need >= n) return false;
        for (int k = 1; k <= need; ++k) {
            BYTE b = p[i + k];
            if ((b & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3F);
        }
        if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000)) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += need + 1;
    }
    return true;
}

static bool HasNul(const BYTE* p, size_t n) {
    for (size_t i = 0; i < n; ++i) if (p[i] == 0) return true;
    return false;
}

static bool IsValidAnsi(const BYTE* p, size_t n) {
    if (n == 0) return true;
    int len = (int)std::min<size_t>(n, INT_MAX / 2);
    if (MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, (LPCCH)p, len, nullptr, 0) > 0)
        return true;
    return GetLastError() != ERROR_NO_UNICODE_TRANSLATION;
}

static Enc DetectEncoding(const BYTE* p, size_t n) {
    Enc e;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        e.kind = Enc::UTF8; e.bom = true; e.skip = 3; return e;
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        e.kind = Enc::UTF16LE; e.bom = true; e.skip = 2; return e;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        e.kind = Enc::UTF16BE; e.bom = true; e.skip = 2; return e;
    }
    size_t probe = std::min(n, (size_t)4096) & ~(size_t)1;
    size_t evenZero = 0, oddZero = 0;
    size_t pairs = probe / 2;
    for (size_t i = 0; i < probe; i += 2) {
        if (p[i] == 0)     ++evenZero;
        if (p[i + 1] == 0) ++oddZero;
    }
    if (pairs > 8) {
        if (oddZero > pairs * 7 / 10 && evenZero < pairs / 10) { e.kind = Enc::UTF16LE; return e; }
        if (evenZero > pairs * 7 / 10 && oddZero < pairs / 10) { e.kind = Enc::UTF16BE; return e; }
    }
    if (IsValidUTF8(p, n) && !HasNul(p, n)) { e.kind = Enc::UTF8; return e; }
    if (IsValidAnsi(p, n) && !HasNul(p, n)) { e.kind = Enc::ANSI;  return e; }
    e.kind = Enc::BYTES;
    return e;
}

static std::wstring Decode(const BYTE* p, size_t n, const Enc& e) {
    const BYTE* q = p + e.skip;
    size_t m = n - e.skip;
    if (m == 0) return std::wstring();

    if (e.kind == Enc::BYTES) {
        std::wstring w(m, 0);
        for (size_t i = 0; i < m; ++i) w[i] = (wchar_t)q[i];
        return w;
    }
    if (e.kind == Enc::UTF16LE || e.kind == Enc::UTF16BE) {
        size_t cnt = m / 2;
        std::wstring w(cnt, 0);
        for (size_t i = 0; i < cnt; ++i) {
            wchar_t c = (wchar_t)(q[i * 2] | (q[i * 2 + 1] << 8));
            if (e.kind == Enc::UTF16BE) c = (wchar_t)(((c >> 8) & 0xFF) | (c << 8));
            w[i] = c;
        }
        return w;
    }
    int cp = (e.kind == Enc::UTF8) ? CP_UTF8 : CP_ACP;
    int need = MultiByteToWideChar(cp, 0, (LPCCH)q, (int)m, nullptr, 0);
    std::wstring w;
    if (need > 0) {
        w.resize((size_t)need);
        MultiByteToWideChar(cp, 0, (LPCCH)q, (int)m, &w[0], need);
    }
    return w;
}

// 把整段文本切成行，同时统计换行风格
static void BuildLines(std::wstring& all, size_t hint) {
    g.lines.clear();
    if (hint > 1) g.lines.reserve(hint);
    g.endsWithNL = (!all.empty() && all.back() == L'\n');
    size_t start = 0, nlCount = 0, crCount = 0, lfOnly = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] == L'\n') {
            g.lines.emplace_back(all.data() + start, i - start);
            if (i > start && all[i - 1] == L'\r') ++crCount;
            else ++lfOnly;
            ++nlCount;
            start = i + 1;
        }
    }
    if (start < all.size()) g.lines.emplace_back(all.data() + start, all.size() - start);
    if (g.lines.empty()) g.lines.emplace_back();
    g.crlfStyle = (nlCount == 0) || (crCount * 2 >= nlCount);
    if (nlCount == 0)                       g.eolName = g.crlfStyle ? L"CRLF" : L"LF";
    else if (lfOnly == 0)                   g.eolName = L"CRLF";
    else if (crCount == 0)                  g.eolName = L"LF";
    else                                    g.eolName = L"混合";
    std::wstring().swap(all);
}

// -----------------------------------------------------------------------------
// 写文件
// -----------------------------------------------------------------------------
struct Writer {
    HANDLE h;
    std::string buf;
    bool ok = true;
    explicit Writer(HANDLE fh) : h(fh) { buf.reserve(1 << 16); }
    void Put(const void* p, size_t n) {
        buf.append((const char*)p, n);
        if (buf.size() >= (1 << 16)) Flush();
    }
    void Flush() {
        if (buf.empty()) return;
        DWORD w = 0;
        if (!WriteFile(h, buf.data(), (DWORD)buf.size(), &w, nullptr) || w != buf.size())
            ok = false;
        buf.clear();
    }
};

static void WriteEncoded(Writer& w, const std::wstring& s, const Enc& e) {
    if (s.empty()) return;
    if (e.kind == Enc::BYTES) {
        std::string out;
        out.reserve(s.size());
        for (wchar_t c : s) {
            if (c <= 0xFF) {
                out.push_back((char)(unsigned char)c);
            } else {
                char b[8];
                int k = WideCharToMultiByte(CP_UTF8, 0, &c, 1, b, 8, nullptr, nullptr);
                if (k <= 0) { out.push_back('?'); }
                else out.append(b, (size_t)k);
            }
        }
        w.Put(out.data(), out.size());
        return;
    }
    if (e.kind == Enc::UTF16LE || e.kind == Enc::UTF16BE) {
        std::wstring o = s;
        if (e.kind == Enc::UTF16BE)
            for (wchar_t& c : o) c = (wchar_t)(((c >> 8) & 0xFF) | (c << 8));
        w.Put(o.data(), o.size() * sizeof(wchar_t));
        return;
    }
    int cp = (e.kind == Enc::UTF8) ? CP_UTF8 : CP_ACP;
    int need = WideCharToMultiByte(cp, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return;
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(cp, 0, s.data(), (int)s.size(), &out[0], need, nullptr, nullptr);
    w.Put(out.data(), out.size());
}

static void WriteBOM(Writer& w, const Enc& e) {
    if (!e.bom) return;
    if (e.kind == Enc::UTF8) { const char b[] = { (char)0xEF, (char)0xBB, (char)0xBF }; w.Put(b, 3); }
    else if (e.kind == Enc::UTF16LE) { const wchar_t b = 0xFEFF; w.Put(&b, 2); }
    else if (e.kind == Enc::UTF16BE) { const wchar_t b = (wchar_t)0xFFFE; w.Put(&b, 2); }
}

#include <commdlg.h>
#include <objbase.h>

// -----------------------------------------------------------------------------
// 注册表设置
// -----------------------------------------------------------------------------
static bool RegDword(HKEY k, const wchar_t* n, DWORD& out) {
    DWORD sz = sizeof(DWORD), type = 0;
    out = 0;
    return RegQueryValueExW(k, n, nullptr, &type, (LPBYTE)&out, &sz) == ERROR_SUCCESS &&
           type == REG_DWORD;
}

static void RegPut(HKEY k, const wchar_t* n, DWORD v) {
    RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}

static void LoadSettings() {
    HKEY k = nullptr;
    g.winRect = RECT{ 0, 0, 0, 0 };
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    DWORD v = 0;
    if (RegDword(k, L"Wrap", v))    g.wrap = v != 0;
    if (RegDword(k, L"TopMost", v)) g.topmost = v != 0;
    if (RegDword(k, L"ShowEol", v)) g.showEol = v != 0;
    if (RegDword(k, L"FontPx", v))  g.fontPx = (int)(v < 8 ? 8 : (v > 48 ? 48 : v));
    LONG x = 0, yy = 0, w = 0, h = 0;
    DWORD sz = sizeof(DWORD), type = 0;
    if (RegQueryValueExW(k, L"WinX", nullptr, &type, (LPBYTE)&x, &sz) == ERROR_SUCCESS &&
        RegQueryValueExW(k, L"WinY", nullptr, &type, (LPBYTE)&yy, &sz) == ERROR_SUCCESS &&
        RegQueryValueExW(k, L"WinW", nullptr, &type, (LPBYTE)&w, &sz) == ERROR_SUCCESS &&
        RegQueryValueExW(k, L"WinH", nullptr, &type, (LPBYTE)&h, &sz) == ERROR_SUCCESS &&
        w > 200 && h > 150) {
        g.winRect = RECT{ (LONG)x, (LONG)yy, (LONG)x + (LONG)w, (LONG)yy + (LONG)h };
    }
    if (RegDword(k, L"Max", v)) g.winMax = v != 0;
    RegCloseKey(k);
}

static void SaveSettings() {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegPut(k, L"Wrap", g.wrap ? 1 : 0);
    RegPut(k, L"TopMost", g.topmost ? 1 : 0);
    RegPut(k, L"ShowEol", g.showEol ? 1 : 0);
    RegPut(k, L"FontPx", (DWORD)(g.dpi ? MulDiv(g.fontPx, 96, g.dpi) : g.fontPx));
    if (g.winRect.right > g.winRect.left && g.winRect.bottom > g.winRect.top) {
        RegPut(k, L"WinX", (DWORD)(LONG)g.winRect.left);
        RegPut(k, L"WinY", (DWORD)(LONG)g.winRect.top);
        RegPut(k, L"WinW", (DWORD)(LONG)(g.winRect.right - g.winRect.left));
        RegPut(k, L"WinH", (DWORD)(LONG)(g.winRect.bottom - g.winRect.top));
    }
    RegPut(k, L"Max", g.winMax ? 1 : 0);
    RegCloseKey(k);
}

// -----------------------------------------------------------------------------
// 长路径（\\?\ 前缀）
// -----------------------------------------------------------------------------
static std::wstring ExtPath(const std::wstring& p) {
    if (p.empty()) return p;
    if (p.size() >= 4 && p.compare(0, 4, L"\\\\?\\") == 0) return p;
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(p.c_str(), 32768, buf, nullptr);
    if (n == 0 || n >= 32768) return p;
    std::wstring f(buf, n);
    if (f.size() >= 2 && f[0] == L'\\' && f[1] == L'\\')
        return L"\\\\?\\UNC\\" + f.substr(2);
    return L"\\\\?\\" + f;
}

static HANDLE OpenRead(const std::wstring& p) {
    std::wstring e = ExtPath(p);
    return CreateFileW(e.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

static bool GetStamp(const std::wstring& p, FILETIME& ft, unsigned long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(ExtPath(p).c_str(), GetFileExInfoStandard, &fad)) return false;
    ft = fad.ftLastWriteTime;
    size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return true;
}

static void UpdateStamp() {
    FILETIME ft;
    unsigned long long sz = 0;
    if (!g.path.empty() && GetStamp(g.path, ft, sz)) {
        g.stamp = ft;
        g.hasStamp = true;
        g.fileSize = sz;
    } else {
        g.hasStamp = false;
    }
}

static void ClampToWorkArea(RECT& r) {
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(mon, &mi)) return;
    const RECT& wa = mi.rcWork;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w > wa.right - wa.left) w = wa.right - wa.left;
    if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
    bool off = r.right < wa.left + 80 || r.left > wa.right - 80 ||
               r.bottom < wa.top + 60 || r.top > wa.bottom - 60;
    if (off) {
        r.left = wa.left + ((wa.right - wa.left) - w) / 2;
        r.top  = wa.top + ((wa.bottom - wa.top) - h) / 3;
    } else {
        if (r.left + w > wa.right) r.left = wa.right - w;
        if (r.top + h > wa.bottom) r.top = wa.bottom - h;
        if (r.left < wa.left) r.left = wa.left;
        if (r.top < wa.top) r.top = wa.top;
    }
    r.right = r.left + w;
    r.bottom = r.top + h;
}

// -----------------------------------------------------------------------------
// 文档文本 <-> 行
// -----------------------------------------------------------------------------
static std::wstring DocumentText() {
    size_t total = 0;
    for (const auto& l : g.lines) total += l.size() + 1;
    std::wstring out;
    out.reserve(total);
    for (size_t i = 0; i < g.lines.size(); ++i) {
        if (i) out += L'\n';
        out += g.lines[i];
    }
    return out;
}

static void SetDocumentText(std::wstring t, size_t hint) {
    BuildLines(t, hint);
}

// 光标位置 + 文本 -> 结束位置
static void PosAfter(size_t l, size_t c, const std::wstring& t, size_t& ol, size_t& oc) {
    size_t nl = 0, last = 0;
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i] == L'\n') { ++nl; last = i + 1; }
    ol = l + nl;
    oc = nl ? (t.size() - last) : (c + t.size());
}

// -----------------------------------------------------------------------------
// 编辑核心：所有修改都走 EditRange，自动记录撤销
// -----------------------------------------------------------------------------
static std::wstring RangeText(size_t l1, size_t c1, size_t l2, size_t c2) {
    if (l1 > l2 || (l1 == l2 && c1 >= c2)) return std::wstring();
    std::wstring out;
    out.append(g.lines[l1], c1, std::wstring::npos);
    for (size_t i = l1 + 1; i <= l2; ++i) {
        out += L'\n';
        if (i == l2) out.append(g.lines[i].data(), c2);
        else out += g.lines[i];
    }
    return out;
}

static void ApplyReplace(size_t l1, size_t c1, size_t l2, size_t c2, const std::wstring& ins) {
    const std::wstring cr = g.crlfStyle ? L"\r" : L"";
    std::vector<std::wstring> parts;
    size_t st = 0;
    for (size_t i = 0; i < ins.size(); ++i)
        if (ins[i] == L'\n') { parts.emplace_back(ins.data() + st, i - st); st = i + 1; }
    parts.emplace_back(ins.data() + st, ins.size() - st);
    // 兼容传入文本本身带 CRLF：去掉每个片段尾部的 '\r'，避免出现 "\r\r"
    for (auto& part : parts)
        if (!part.empty() && part.back() == L'\r') part.pop_back();
    std::wstring prefix = g.lines[l1].substr(0, c1);
    std::wstring suffix = g.lines[l2].substr(c2);
    std::vector<std::wstring> nl;
    if (parts.size() == 1) {
        nl.push_back(prefix + parts[0] + suffix);
    } else {
        nl.push_back(prefix + parts[0] + cr);
        for (size_t i = 1; i + 1 < parts.size(); ++i) nl.push_back(parts[i] + cr);
        nl.push_back(parts.back() + suffix);
    }
    g.lines.erase(g.lines.begin() + (ptrdiff_t)l1,
                  g.lines.begin() + (ptrdiff_t)l2 + 1);
    g.lines.insert(g.lines.begin() + (ptrdiff_t)l1, nl.begin(), nl.end());
    g.curL = l1 + nl.size() - 1;
    g.curC = (parts.size() == 1) ? c1 + parts[0].size() : parts.back().size();
    if (g.lines.size() == 1 && g.lines[0].empty()) g.endsWithNL = false;
}

static void EditRange(size_t l1, size_t c1, size_t l2, size_t c2,
                      const std::wstring& ins, bool coalesce) {
    if (g.lines.empty()) g.lines.emplace_back();
    if (l1 > l2 || (l1 == l2 && c1 > c2)) {
        std::swap(l1, l2); std::swap(c1, c2);
    }
    if (l1 >= g.lines.size()) return;
    if (l2 >= g.lines.size()) l2 = g.lines.size() - 1;
    c1 = std::min(c1, VLen(g.lines[l1]));
    c2 = std::min(c2, VLen(g.lines[l2]));
    if (l1 == l2 && c1 == c2 && ins.empty()) return;

    // 连续单字符输入合并为一步撤销
    if (coalesce && ins.size() == 1 && ins[0] >= L' ' && !g.hasSel && !g.undo.empty()) {
        UndoRec& last = g.undo.back();
        if (last.oldText.empty() && last.c1 == last.c2 && last.l1 == last.l2 &&
            last.l1 == l1 && l1 == l2 && c1 == c2 &&
            !last.newText.empty() && last.newText.size() < 96 &&
            last.newText.find(L'\n') == std::wstring::npos &&
            c1 == last.c1 + last.newText.size() &&
            GetTickCount64() - g.lastEditTick < 900) {
            ApplyReplace(l1, c1, l2, c2, ins);
            last.newText += ins;
            last.caretAfterL = g.curL;
            last.caretAfterC = g.curC;
            g.redo.clear();
            g.lastEditTick = GetTickCount64();
            g.hasSel = false;
            g.anchorL = g.curL; g.anchorC = g.curC;
            AfterEdit();
            return;
        }
    }

    UndoRec r;
    r.l1 = l1; r.c1 = c1; r.l2 = l2; r.c2 = c2;
    r.oldText = RangeText(l1, c1, l2, c2);
    r.newText = ins;
    r.caretBeforeL = g.curL; r.caretBeforeC = g.curC;
    ApplyReplace(l1, c1, l2, c2, ins);
    r.caretAfterL = g.curL; r.caretAfterC = g.curC;
    g.undo.push_back(std::move(r));
    if (g.undo.size() > 4000) g.undo.erase(g.undo.begin(), g.undo.begin() + 500);
    g.redo.clear();
    g.lastEditTick = GetTickCount64();
    g.hasSel = false;
    g.anchorL = g.curL; g.anchorC = g.curC;
    AfterEdit();
}

static void UndoRedo(bool redo) {
    std::vector<UndoRec>& src = redo ? g.redo : g.undo;
    if (src.empty()) return;
    UndoRec r = std::move(src.back());
    src.pop_back();
    const std::wstring& cur  = redo ? r.oldText : r.newText;
    const std::wstring& want = redo ? r.newText : r.oldText;
    size_t el = r.l1, ec = r.c1;
    PosAfter(r.l1, r.c1, cur, el, ec);
    if (el >= g.lines.size()) el = g.lines.size() - 1;
    ApplyReplace(r.l1, r.c1, el, ec, want);

    UndoRec inv;
    inv.l1 = r.l1; inv.c1 = r.c1;
    PosAfter(r.l1, r.c1, want, inv.l2, inv.c2);
    inv.oldText = want;
    inv.newText = cur;
    size_t setL, setC;
    if (redo) {
        setL = r.caretAfterL; setC = r.caretAfterC;
        inv.caretBeforeL = setL; inv.caretBeforeC = setC;
        inv.caretAfterL = r.caretBeforeL; inv.caretAfterC = r.caretBeforeC;
    } else {
        setL = r.caretBeforeL; setC = r.caretBeforeC;
        inv.caretBeforeL = r.caretBeforeL; inv.caretBeforeC = r.caretBeforeC;
        inv.caretAfterL = r.caretAfterL; inv.caretAfterC = r.caretAfterC;
    }
    g.curL = std::min(setL, g.lines.size() - 1);
    g.curC = std::min(setC, VLen(g.lines[g.curL]));
    g.hasSel = false;
    g.anchorL = g.curL; g.anchorC = g.curC;
    g.desiredX = -1;
    (redo ? g.undo : g.redo).push_back(std::move(inv));
    AfterEdit();
}

// -----------------------------------------------------------------------------
// 文件对话框（现代 COM 对话框，支持长路径；失败时退回旧对话框）
// -----------------------------------------------------------------------------
static bool PickOpen(std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr)) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        COMDLG_FILTERSPEC types[] = {
            { L"所有文件 (*.*)", L"*.*" },
            { L"文本文件 (*.txt)", L"*.txt" },
        };
        dlg->SetFileTypes(2, types);
        if (!g.path.empty()) {
            IShellItem* folder = nullptr;
            std::wstring dir = DirName(g.path);
            if (!dir.empty() &&
                SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr,
                                                      IID_PPV_ARGS(&folder))) && folder) {
                dlg->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(g.hwnd))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it)) && it) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    out.assign(p);
                    CoTaskMemFree(p);
                }
                it->Release();
            }
        }
        dlg->Release();
        return !out.empty();
    }
    wchar_t file[32768] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0文本文件 (*.txt)\0*.txt\0\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out.assign(file);
    return true;
}

static bool PickSave(const std::wstring& suggest, std::wstring& out) {
    IFileSaveDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr)) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
        COMDLG_FILTERSPEC types[] = {
            { L"文本文件 (*.txt)", L"*.txt" },
            { L"所有文件 (*.*)", L"*.*" },
        };
        dlg->SetFileTypes(2, types);
        dlg->SetDefaultExtension(L"txt");
        if (!suggest.empty()) dlg->SetFileName(suggest.c_str());
        if (!g.path.empty()) {
            IShellItem* folder = nullptr;
            std::wstring dir = DirName(g.path);
            if (!dir.empty() &&
                SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr,
                                                      IID_PPV_ARGS(&folder))) && folder) {
                dlg->SetFolder(folder);
                folder->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(g.hwnd))) {
            IShellItem* it = nullptr;
            if (SUCCEEDED(dlg->GetResult(&it)) && it) {
                PWSTR p = nullptr;
                if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    out.assign(p);
                    CoTaskMemFree(p);
                }
                it->Release();
            }
        }
        dlg->Release();
        return !out.empty();
    }
    wchar_t file[32768] = { 0 };
    if (!suggest.empty()) wcsncpy(file, suggest.c_str(), 32767);
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"文本文件 (*.txt)\0*.txt\0所有文件 (*.*)\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    out.assign(file);
    return true;
}

// -----------------------------------------------------------------------------
// 打开 / 保存
// -----------------------------------------------------------------------------
static bool AskSavePath() {
    std::wstring suggest = g.path.empty() ? std::wstring(L"未命名.txt") : FileName(g.path);
    std::wstring out;
    if (!PickSave(suggest, out)) return false;
    g.path = out;
    return true;
}

static bool ShouldOpenNewWindow() {
    if (!g.path.empty() || g.dirty) return true;
    if (g.lines.size() > 1) return true;
    if (g.lines.size() == 1 && !g.lines[0].empty()) return true;
    return false;
}

static void LaunchNewWindow(const std::wstring& p) {
    wchar_t exe[32768] = { 0 };
    if (!GetModuleFileNameW(nullptr, exe, 32768)) return;
    std::wstring cmd = L"\"";
    cmd += exe;
    cmd += L"\" \"";
    cmd += p;
    cmd += L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (CreateProcessW(exe, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static void OpenDialog() {
    std::wstring p;
    if (!PickOpen(p)) return;
    if (ShouldOpenNewWindow()) LaunchNewWindow(p);
    else LoadFile(p);
}

// 重新按指定编码读取磁盘文件；forceKind < 0 表示自动检测
static void ReloadWithEncoding(int forceKind);

static void LoadFile(const std::wstring& p, int forceKind, bool skipDiscard) {
    if (!skipDiscard && !ConfirmDiscard()) return;

    HANDLE hf = OpenRead(p);
    if (hf == INVALID_HANDLE_VALUE) {
        MessageBoxW(g.hwnd, L"无法打开该文件。", L"Bare", MB_OK | MB_ICONWARNING);
        return;
    }
    LARGE_INTEGER li = { { 0, 0 } };
    GetFileSizeEx(hf, &li);
    size_t n = (size_t)li.QuadPart;

    if (li.QuadPart > (LONGLONG)INT_MAX - 8) {
        MessageBoxW(g.hwnd, L"文件超过 2 GB，已跳过。", L"Bare", MB_OK | MB_ICONWARNING);
        CloseHandle(hf);
        return;
    }
    if (n > 512ULL * 1024 * 1024) {
        if (MessageBoxW(g.hwnd, L"文件超过 512 MB，加载会占用较多内存，是否继续？",
                        L"Bare", MB_YESNO | MB_ICONWARNING) != IDYES) {
            CloseHandle(hf);
            return;
        }
    }

    const BYTE* data = nullptr;
    HANDLE hm = nullptr;
    if (n > 0) {
        hm = CreateFileMappingW(hf, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (hm) data = (const BYTE*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
        if (!data) {
            if (hm) CloseHandle(hm);
            CloseHandle(hf);
            MessageBoxW(g.hwnd, L"无法读取文件内容。", L"Bare", MB_OK | MB_ICONWARNING);
            return;
        }
    }

    Enc e = (forceKind >= 0) ? Enc() : DetectEncoding(data, n);
    if (forceKind >= 0) {
        e.kind = (Enc::Kind)forceKind;
        e.bom = (forceKind == Enc::UTF8 || forceKind == Enc::UTF16LE ||
                 forceKind == Enc::UTF16BE);
        if (forceKind == Enc::UTF8 && n >= 3 && data && data[0] == 0xEF && data[1] == 0xBB &&
            data[2] == 0xBF) e.skip = 3;
        else if (forceKind == Enc::UTF16LE && n >= 2 && data && data[0] == 0xFF && data[1] == 0xFE) e.skip = 2;
        else if (forceKind == Enc::UTF16BE && n >= 2 && data && data[0] == 0xFE && data[1] == 0xFF) e.skip = 2;
    }

    size_t hint = 1;
    if (data && n > e.skip) {
        if (e.kind == Enc::UTF8 || e.kind == Enc::ANSI || e.kind == Enc::BYTES) {
            const char* s = (const char*)data + e.skip;
            const char* end = s + (n - e.skip);
            while (s < end) {
                const char* f = (const char*)memchr(s, '\n', (size_t)(end - s));
                if (!f) break;
                ++hint;
                s = f + 1;
            }
        } else {
            hint = (n - e.skip) / 80 + 1;
        }
    }

    HCURSOR waitCur = LoadCursorW(nullptr, IDC_WAIT);
    HCURSOR oldCur = SetCursor(waitCur);
    std::wstring all = Decode(data, n, e);
    if (hm) { UnmapViewOfFile(data); CloseHandle(hm); }
    CloseHandle(hf);
    BuildLines(all, hint);
    SetCursor(oldCur);

    g.path = p;
    g.enc = e;
    g.fileSize = li.QuadPart;
    g.dirty = false;
    g.curL = g.curC = 0;
    g.hasSel = false;
    g.selecting = false;
    g.pendingHigh = 0;
    g.desiredX = -1;
    g.topL = 0;
    g.xOffset = 0;
    g.maxLinePx = 0;
    g.undo.clear();
    g.redo.clear();
    g.savedDepth = 0;
    ResetFind();
    UpdateStamp();
    UpdateTitle();
    UpdateScroll();
    Refresh();
}

// 以强制编码重新读取当前文件
static void ReloadWithEncoding(int forceKind) {
    if (g.path.empty()) {
        if (forceKind < 0) return;
        g.enc.kind = (Enc::Kind)forceKind;
        g.enc.bom = (forceKind == Enc::UTF8 || forceKind == Enc::UTF16LE ||
                     forceKind == Enc::UTF16BE);
        g.dirty = true;
        UpdateDirty();
        Refresh();
        return;
    }
    if (forceKind < 0) {
        if (!ConfirmDiscard()) return;
        LoadFile(g.path, -1);
    } else {
        LoadFile(g.path, forceKind);
    }
}

static bool WriteDocumentTo(const std::wstring& target, bool withBackup, std::wstring& err) {
    std::wstring tmp = target + L".bare-tmp";
    HANDLE hf = CreateFileW(ExtPath(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        err = L"无法写入文件：路径不可写或文件被占用。";
        return false;
    }
    Writer w(hf);
    WriteBOM(w, g.enc);
    const std::wstring nl = L"\n";
    for (size_t i = 0; i < g.lines.size() && w.ok; ++i) {
        WriteEncoded(w, g.lines[i], g.enc);
        if (i + 1 < g.lines.size() || g.endsWithNL) WriteEncoded(w, nl, g.enc);
    }
    w.Flush();
    FlushFileBuffers(hf);
    CloseHandle(hf);
    if (!w.ok) {
        DeleteFileW(ExtPath(tmp).c_str());
        err = L"写入文件时发生错误。";
        return false;
    }

    std::wstring t = ExtPath(target);
    std::wstring tp = ExtPath(tmp);
    bool exists = GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool ok;
    if (exists) {
        std::wstring bak;
        LPCWSTR bakPtr = nullptr;
        if (withBackup) {
            bak = ExtPath(target + L".bak");
            bakPtr = bak.c_str();
        }
        ok = ReplaceFileW(t.c_str(), tp.c_str(), bakPtr,
                          REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != 0;
    } else {
        ok = MoveFileExW(tp.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!ok) {
        DeleteFileW(tp.c_str());
        err = L"替换原文件失败（可能被其他程序占用）。";
        return false;
    }
    return true;
}

static void SaveFile(bool saveAs) {
    if (saveAs || g.path.empty()) {
        if (!AskSavePath()) return;
    }
    // 文件在磁盘上被外部修改过？覆盖前确认
    if (g.hasStamp) {
        FILETIME ft;
        unsigned long long sz = 0;
        if (GetStamp(g.path, ft, sz) &&
            (CompareFileTime(&ft, &g.stamp) != 0 || sz != g.fileSize)) {
            if (MessageBoxW(g.hwnd,
                            L"该文件在磁盘上已被其他程序修改。\n继续保存会覆盖这些外部修改，是否继续？",
                            L"Bare", MB_YESNO | MB_ICONWARNING) != IDYES)
                return;
        }
    }
    std::wstring err;
    if (!WriteDocumentTo(g.path, true, err)) {
        MessageBoxW(g.hwnd, err.c_str(), L"Bare", MB_OK | MB_ICONWARNING);
        return;
    }
    UpdateStamp();
    g.savedDepth = g.undo.size();
    g.redo.clear();
    g.dirty = false;
    UpdateDirty();
    Flash(L"已保存");
    UpdateTitle();
    Refresh();
}

static bool ConfirmDiscard() {
    if (!g.dirty) return true;
    std::wstring name = g.path.empty() ? std::wstring(L"未命名") : FileName(g.path);
    std::wstring msg = L"「" + name + L"」有未保存的修改，是否保存？";
    int r = MessageBoxW(g.hwnd, msg.c_str(), L"Bare",
                        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) {
        SaveFile(false);
        if (g.dirty) return false;
    }
    return true;
}

static void NewFile() {
    if (!ConfirmDiscard()) return;
    g.path.clear();
    g.enc = Enc();
    g.dirty = false;
    g.fileSize = 0;
    g.endsWithNL = false;
    g.crlfStyle = true;
    g.eolName = L"CRLF";
    g.lines.clear();
    g.lines.emplace_back();
    g.curL = g.curC = 0;
    g.hasSel = false;
    g.selecting = false;
    g.pendingHigh = 0;
    g.desiredX = -1;
    g.topL = 0;
    g.xOffset = 0;
    g.maxLinePx = 0;
    g.undo.clear();
    g.redo.clear();
    g.savedDepth = 0;
    g.hasStamp = false;
    ResetFind();
    UpdateTitle();
    UpdateScroll();
    Refresh();
}

// -----------------------------------------------------------------------------
// 视图布局（支持自动换行）
// -----------------------------------------------------------------------------
static int TextTop() {
    return g.toolbarH + (g.find.open ? g.findH : 0);
}

static int TextBottom() {
    return g.clientH - g.statusH;
}

static int VisLines() {
    return std::max(1, (TextBottom() - TextTop()) / g.lineH);
}

static int MaxTop() {
    size_t n = g.lines.empty() ? 0 : g.lines.size() - 1;
    return (int)std::min<size_t>(n, INT_MAX);
}

static int FitWidth() {
    int w = g.clientW - g.gutterW - MulDiv(24, g.dpi, 96);
    return std::max(g.charW * 4, w);
}

// 测量专用 DC（屏幕兼容），避免反复 GetDC
static HDC MeasureDC() {
    static HDC h = nullptr;
    if (!h) h = CreateCompatibleDC(nullptr);
    if (g.hMono) SelectObject(h, g.hMono);
    return h;
}

// 计算一行的显示串宽度前缀与换行断点（breaks[0] = 0）
static void LayoutLine(HDC hdc, size_t l, std::vector<int>& dx,
                       std::vector<size_t>& breaks) {
    breaks.clear();
    breaks.push_back(0);
    static std::wstring d;
    d = Disp(g.lines[l]);
    ComputeDx(hdc, d, dx);
    if (!g.wrap || d.size() < 2) return;
    const int fit = FitWidth();
    size_t start = 0;
    for (size_t c = 1; c < d.size(); ++c) {
        if (dx[c + 1] - dx[start] > fit) {
            breaks.push_back(c);
            start = c;
        }
    }
}

static int LineRows(HDC hdc, size_t l) {
    if (!g.wrap) return 1;
    if (l >= g.lines.size()) return 1;
    std::vector<int> dx;
    std::vector<size_t> br;
    LayoutLine(hdc, l, dx, br);
    return (int)br.size();
}

// 光标在行内的段号
static int CaretSeg(const std::vector<size_t>& breaks, size_t col) {
    int seg = 0;
    for (size_t k = 0; k < breaks.size(); ++k)
        if (breaks[k] <= col) seg = (int)k;
    return seg;
}

static int CaretXIn(HDC hdc) {
    std::wstring d = Disp(g.lines[g.curL]);
    std::vector<int> dx;
    ComputeDx(hdc, d, dx);
    size_t c = std::min(g.curC, d.size());
    return dx[c];
}

static void UpdateScroll() {
    SCROLLINFO si = { 0 };
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = MaxTop();
    si.nPage = (UINT)VisLines();
    si.nPos = (int)std::min<size_t>(g.topL, INT_MAX);
    SetScrollInfo(g.hwnd, SB_VERT, &si, TRUE);
}

static void UpdateTitle() {
    std::wstring t = (g.path.empty() ? L"未命名" : FileName(g.path));
    if (g.dirty) t += L" *";
    t += L" — Bare";
    SetWindowTextW(g.hwnd, t.c_str());
}

static void UpdateDirty() {
    bool d = (g.undo.size() != g.savedDepth);
    if (d != g.dirty) {
        g.dirty = d;
        UpdateTitle();
    }
}

static void Refresh() {
    EnsureCaretVisible();
    UpdateScroll();
    UpdateImePos();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void AfterMove(bool resetDesired) {
    if (resetDesired) g.desiredX = -1;
    g.pendingHigh = 0;
    Refresh();
}

static void AfterEdit() {
    if (g.lines.empty()) g.lines.emplace_back();
    g.desiredX = -1;
    g.pendingHigh = 0;
    if (g.find.open) {
        g.find.dirtyHits = true;
        g.find.current = -1;
        if (g.hwnd) SetTimer(g.hwnd, 2, 90, nullptr);
    }
    UpdateDirty();
    UpdateTitle();
    Refresh();
}

static void Flash(const wchar_t* s) {
    g.flash = s;
    g.flashUntil = GetTickCount64() + 1600;
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void EnsureCaretVisible() {
    if (g.lines.empty()) return;
    g.curL = std::min(g.curL, g.lines.size() - 1);
    g.curC = std::min(g.curC, VLen(g.lines[g.curL]));
    if (g.wrap) g.xOffset = 0;

    if (g.curL < g.topL) g.topL = g.curL;
    HDC hdc = MeasureDC();
    int vis = VisLines();

    // 计算光标行距顶部的可视行数
    int rows = 0;
    if (g.wrap) {
        for (size_t i = g.topL; i < g.curL; ++i) rows += LineRows(hdc, i);
        std::vector<int> dx;
        std::vector<size_t> br;
        LayoutLine(hdc, g.curL, dx, br);
        rows += CaretSeg(br, g.curC);
    } else {
        rows = (int)(g.curL - g.topL);
    }
    while (rows >= vis && g.topL < g.curL) {
        if (g.wrap) rows -= LineRows(hdc, g.topL);
        else --rows;
        ++g.topL;
    }
    g.topL = (size_t)std::clamp((int)std::min<size_t>(g.topL, INT_MAX), 0, MaxTop());

    // 横向（仅不换行时）
    if (!g.wrap) {
        int cx = CaretXIn(hdc);
        int viewW = std::max(40, g.clientW - g.gutterW - 10);
        if (cx - g.xOffset < 0) g.xOffset = cx;
        if (cx - g.xOffset > viewW - 4) g.xOffset = cx - viewW + 40;
        if (g.xOffset < 0) g.xOffset = 0;
    }
}

// 光标在屏幕上的像素位置（左上角）
static void CaretScreenPos(int& x, int& y) {
    x = g.gutterW; y = TextTop();
    if (g.lines.empty()) return;
    HDC hdc = MeasureDC();
    int yy = TextTop();
    if (g.wrap) {
        for (size_t i = g.topL; i < g.curL; ++i) yy += LineRows(hdc, i) * g.lineH;
    } else {
        yy += (int)(g.curL > g.topL ? g.curL - g.topL : 0) * g.lineH;
    }
    std::vector<int> dx;
    std::vector<size_t> br;
    if (g.curL < g.lines.size()) LayoutLine(hdc, g.curL, dx, br);
    int seg = CaretSeg(br, g.curC);
    size_t segStart = seg < (int)br.size() ? br[seg] : 0;
    size_t c = std::min(g.curC, dx.empty() ? (size_t)0 : dx.size() - 1);
    x = g.gutterW + (dx[c] - dx[segStart]) - g.xOffset;
    y = yy + seg * g.lineH;
}

// 移动光标（自动换行时按可视行移动）
static void MoveVert(int delta, bool ext) {
    if (ext && !g.hasSel) { g.anchorL = g.curL; g.anchorC = g.curC; }
    HDC hdc = MeasureDC();
    if (!g.wrap) {
        if (g.desiredX < 0) g.desiredX = CaretXIn(hdc);
        long long t = (long long)g.curL + delta;
        if (t < 0) t = 0;
        if (t > (long long)g.lines.size() - 1) t = (long long)g.lines.size() - 1;
        size_t nl = (size_t)t;
        std::wstring d = Disp(g.lines[nl]);
        std::vector<int> dx;
        ComputeDx(hdc, d, dx);
        g.curL = nl;
        g.curC = ColFromX(d, dx, g.desiredX);
        g.hasSel = ext;
        AfterMove(false);
        return;
    }

    // 换行模式
    std::vector<int> dx;
    std::vector<size_t> br;
    LayoutLine(hdc, g.curL, dx, br);
    int seg = CaretSeg(br, g.curC);
    int xInSeg = dx[std::min(g.curC, dx.size() - 1)] - dx[br[seg]];
    if (g.desiredX >= 0) xInSeg = g.desiredX;
    while (delta > 0) {
        if (seg + 1 < (int)br.size()) {
            ++seg;
        } else if (g.curL + 1 < g.lines.size()) {
            ++g.curL;
            LayoutLine(hdc, g.curL, dx, br);
            seg = 0;
        } else break;
        --delta;
    }
    while (delta < 0) {
        if (seg > 0) {
            --seg;
        } else if (g.curL > 0) {
            --g.curL;
            LayoutLine(hdc, g.curL, dx, br);
            seg = (int)br.size() - 1;
        } else break;
        ++delta;
    }
    g.desiredX = xInSeg;
    std::wstring d = Disp(g.lines[g.curL]);
    size_t segStart = br[seg];
    size_t segEnd = (seg + 1 < (int)br.size()) ? br[seg + 1] : d.size();
    size_t c = ColFromX(d, dx, dx[segStart] + xInSeg);
    g.curC = std::min(std::max(c, segStart), std::min(segEnd, d.size()));
    g.hasSel = ext;
    AfterMove(false);
}

// -----------------------------------------------------------------------------
// IME
// -----------------------------------------------------------------------------
struct CompositionForm { DWORD dwStyle; POINT ptCurrentPos; RECT rcArea; };

typedef void* (WINAPI* IGC_t)(HWND);
typedef BOOL  (WINAPI* IRC_t)(void*, HWND);
typedef BOOL  (WINAPI* ISCW_t)(void*, void*);
typedef LONG  (WINAPI* IGCS_t)(void*, DWORD, void*, DWORD);

static HMODULE ImeDll() {
    static HMODULE h = LoadLibraryW(L"imm32.dll");
    return h;
}

static bool ImeComposing() {
    HMODULE h = ImeDll();
    if (!h) return false;
    static IGC_t  getCtx = (IGC_t)GetProcAddress(h, "ImmGetContext");
    static IRC_t  relCtx = (IRC_t)GetProcAddress(h, "ImmReleaseContext");
    static IGCS_t getStr = (IGCS_t)GetProcAddress(h, "ImmGetCompositionStringW");
    if (!getCtx || !relCtx || !getStr) return false;
    void* imc = getCtx(g.hwnd);
    if (!imc) return false;
    LONG n = getStr(imc, 0x0008 /* GCS_COMPSTR */, nullptr, 0);
    relCtx(imc, g.hwnd);
    return n > 0;
}

static void UpdateImePos() {
    HMODULE h = ImeDll();
    if (!h) return;
    static IGC_t  getCtx = (IGC_t)GetProcAddress(h, "ImmGetContext");
    static IRC_t  relCtx = (IRC_t)GetProcAddress(h, "ImmReleaseContext");
    static ISCW_t setCW  = (ISCW_t)GetProcAddress(h, "ImmSetCompositionWindow");
    if (!getCtx || !relCtx || !setCW) return;
    void* imc = getCtx(g.hwnd);
    if (!imc) return;
    int cx = 0, cy = 0;
    CaretScreenPos(cx, cy);
    CompositionForm cf = { 0 };
    cf.dwStyle = 0x0002; // CFS_POINT
    cf.ptCurrentPos.x = cx;
    cf.ptCurrentPos.y = cy + g.lineH;
    cf.rcArea.left = g.gutterW;
    cf.rcArea.top = TextTop();
    cf.rcArea.right = g.clientW;
    cf.rcArea.bottom = TextBottom();
    setCW(imc, &cf);
    relCtx(imc, g.hwnd);
}

// -----------------------------------------------------------------------------
// 查找 / 替换
// -----------------------------------------------------------------------------
static bool MatchAt(const std::wstring& s, size_t pos, const std::wstring& q) {
    if (pos + q.size() > s.size()) return false;
    if (g.find.caseSensitive)
        return wmemcmp(s.data() + pos, q.data(), q.size()) == 0;
    return _wcsnicmp(s.data() + pos, q.data(), q.size()) == 0;
}

static void CollectLineHits(size_t line, std::vector<size_t>& cols) {
    cols.clear();
    const std::wstring& ln = g.lines[line];
    const std::wstring& q = g.find.q;
    size_t vl = VLen(ln);
    if (q.empty() || vl < q.size()) return;
    size_t last = vl - q.size();
    for (size_t c = 0; c <= last; ++c) {
        if (!MatchAt(ln, c, q)) continue;
        cols.push_back(c);
        c += q.size() - 1;
    }
}

static void RebuildFindHits() {
    FindState& F = g.find;
    F.hits.clear();
    F.current = -1;
    F.truncated = false;
    F.dirtyHits = false;
    if (F.q.empty()) return;
    const size_t kCap = 100000;
    std::vector<size_t> cols;
    for (size_t l = 0; l < g.lines.size(); ++l) {
        CollectLineHits(l, cols);
        for (size_t c : cols) {
            F.hits.emplace_back(l, c);
            if (F.hits.size() >= kCap) { F.truncated = true; return; }
        }
    }
}

static void SelectHit(size_t idx) {
    FindState& F = g.find;
    if (idx >= F.hits.size()) return;
    F.current = (int)idx;
    g.anchorL = F.hits[idx].first;
    g.anchorC = F.hits[idx].second;
    g.curL = F.hits[idx].first;
    g.curC = F.hits[idx].second + F.q.size();
    g.hasSel = true;
    g.desiredX = -1;
    Refresh();
}

static void FindNav(bool forward) {
    FindState& F = g.find;
    if (F.q.empty()) { InvalidateRect(g.hwnd, nullptr, FALSE); return; }
    if (F.dirtyHits) RebuildFindHits();
    if (F.hits.empty()) {
        F.current = -1;
        InvalidateRect(g.hwnd, nullptr, FALSE);
        return;
    }
    size_t n = F.hits.size(), idx = 0;
    if (F.current < 0 || (size_t)F.current >= n) {
        if (F.fromStart) {
            idx = forward ? 0 : n - 1;
        } else if (forward) {
            bool found = false;
            for (size_t i = 0; i < n; ++i) {
                const auto& h = F.hits[i];
                if (h.first > g.curL || (h.first == g.curL && h.second >= g.curC)) {
                    idx = i; found = true; break;
                }
            }
            if (!found) idx = 0;
        } else {
            idx = 0;
            for (size_t i = n; i-- > 0;) {
                const auto& h = F.hits[i];
                if (h.first < g.curL || (h.first == g.curL && h.second < g.curC)) {
                    idx = i; break;
                }
            }
        }
        SelectHit(idx);
        return;
    }
    idx = forward ? ((size_t)F.current + 1) % n
                  : ((size_t)F.current + n - 1) % n;
    SelectHit(idx);
}

static void OnFindChanged() {
    g.find.dirtyHits = true;
    g.find.current = -1;
    g.find.truncated = false;
    if (g.hwnd) SetTimer(g.hwnd, 2, 90, nullptr);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void SyncFindHeight() {
    int rh  = MulDiv(26, g.dpi, 96);
    int pad = MulDiv(10, g.dpi, 96);
    int gap = MulDiv(6, g.dpi, 96);
    g.findH = pad + rh + (g.find.replaceMode ? gap + rh : 0) + pad;
}

static void OpenFind(bool replaceMode) {
    bool was = g.find.open;
    g.find.open = true;
    if (replaceMode) {
        g.find.replaceMode = true;
        g.find.activeField = 0;
    } else if (!was || g.find.replaceMode) {
        g.find.activeField = 0;
    }
    if (!g.find.replaceMode) g.find.activeField = 0;
    SyncFindHeight();
    g.hasSel = false;
    UpdateScroll();
    EnsureCaretVisible();
    UpdateImePos();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void CloseFind() {
    if (!g.find.open) return;
    g.find.open = false;
    g.find.hits.clear();
    g.find.current = -1;
    g.find.dirtyHits = true;
    g.find.truncated = false;
    SyncFindHeight();
    UpdateScroll();
    EnsureCaretVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void ResetFind() {
    g.find.open = false;
    g.find.replaceMode = false;
    g.find.activeField = 0;
    g.find.q.clear();
    g.find.rep.clear();
    g.find.hits.clear();
    g.find.current = -1;
    g.find.dirtyHits = true;
    g.find.truncated = false;
    SyncFindHeight();
}

static void FindRepeat(bool forward) {
    if (g.find.q.empty()) { OpenFind(false); return; }
    OpenFind(false);
    FindNav(forward);
}

static void PasteToField() {
    std::wstring t;
    if (OpenClipboard(g.hwnd)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p) {
                for (const wchar_t* s = p; *s && t.size() < 400; ++s)
                    if (*s >= 32 && *s != 0x7F) t.push_back(*s);
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    if (t.empty()) return;
    if (g.find.activeField == 0) g.find.q += t;
    else g.find.rep += t;
    OnFindChanged();
}

static void ReplaceCurrent() {
    FindState& F = g.find;
    if (F.q.empty()) return;
    if (F.dirtyHits) RebuildFindHits();
    if (F.current < 0 || (size_t)F.current >= F.hits.size()) {
        FindNav(true);
        if (F.current < 0) return;
    }
    auto h = F.hits[F.current];
    EditRange(h.first, h.second, h.first, h.second + F.q.size(), F.rep, false);
    F.dirtyHits = true;
    F.current = -1;
    FindNav(true);
}

static void ReplaceAll() {
    FindState& F = g.find;
    if (F.q.empty()) return;
    if (F.dirtyHits) RebuildFindHits();
    size_t cnt = F.hits.size();
    if (cnt == 0) { Flash(L"没有可替换的内容"); return; }
    if (cnt > 30000 || g.lines.size() > 200000) {
        std::wstring m = L"即将替换 " + std::to_wstring(cnt) + L" 处内容，是否继续？";
        if (MessageBoxW(g.hwnd, m.c_str(), L"Bare", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    }
    std::wstring oldText = DocumentText();
    std::wstring out;
    out.reserve(oldText.size() + cnt * F.rep.size());
    size_t i = 0;
    while (i < oldText.size()) {
        if (i + F.q.size() <= oldText.size() && MatchAt(oldText, i, F.q)) {
            out += F.rep;
            i += F.q.size();
        } else {
            out.push_back(oldText[i]);
            ++i;
        }
    }
    if (out == oldText) return;
    HCURSOR wc = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    size_t hint = 1;
    for (wchar_t c : out) if (c == L'\n') ++hint;
    g.hasSel = false;
    size_t lastL = g.lines.size() - 1, lastC = VLen(g.lines[lastL]);
    EditRange(0, 0, lastL, lastC, out, false);
    SetCursor(wc);
    OnFindChanged();
    std::wstring msg = std::to_wstring(cnt) + L" 处已替换";
    Flash(msg.c_str());
}

// 查找栏布局
struct FindLayout {
    RECT field, up, down, aa, close;
    RECT repField, repl, replAll, fromStart;
};

static FindLayout FindRects() {
    const int rh   = MulDiv(26, g.dpi, 96);
    const int pad  = MulDiv(10, g.dpi, 96);
    const int gap  = MulDiv(6, g.dpi, 96);
    const int y1   = g.toolbarH + pad;
    const int right = g.clientW - pad;
    FindLayout L = {};
    int x = right;
    L.close = { x - rh, y1, x, y1 + rh }; x -= rh + gap * 2;
    int aw = MulDiv(30, g.dpi, 96);
    L.aa = { x - aw, y1, x, y1 + rh }; x -= aw + gap;
    L.down = { x - rh, y1, x, y1 + rh }; x -= rh + gap;
    L.up = { x - rh, y1, x, y1 + rh }; x -= rh + MulDiv(12, g.dpi, 96);
    int fw = MulDiv(360, g.dpi, 96);
    int avail = x - pad;
    if (fw > avail) fw = std::max(0, avail);
    L.field = { pad, y1, pad + fw, y1 + rh };

    if (g.find.replaceMode) {
        int y2 = y1 + rh + gap;
        int bw1 = MulDiv(58, g.dpi, 96);
        int bw2 = MulDiv(86, g.dpi, 96);
        int bw3 = MulDiv(64, g.dpi, 96);
        int xr = right;
        L.fromStart = { xr - bw3, y2, xr, y2 + rh }; xr -= bw3 + gap;
        L.replAll = { xr - bw2, y2, xr, y2 + rh }; xr -= bw2 + gap;
        L.repl = { xr - bw1, y2, xr, y2 + rh }; xr -= bw1 + MulDiv(12, g.dpi, 96);
        int fw2 = std::max(0, xr - pad);
        L.repField = { pad, y2, pad + fw2, y2 + rh };
    }
    return L;
}

// -----------------------------------------------------------------------------
// 编码 / 换行菜单
// -----------------------------------------------------------------------------
static void ShowEncodingMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"自动检测（重新载入）");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    const wchar_t* names[] = { L"UTF-8", L"UTF-16 LE", L"UTF-16 BE", L"ANSI / GBK", L"字节模式" };
    for (int k = 0; k < 5; ++k) AppendMenuW(m, MF_STRING, 10 + k, names[k]);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                  pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) ReloadWithEncoding(-1);
    else if (cmd >= 10) ReloadWithEncoding(cmd - 10);
}

static void ConvertEol(bool toCrlf) {
    bool changed = false;
    for (size_t i = 0; i < g.lines.size(); ++i) {
        bool hasNL = (i + 1 < g.lines.size()) || g.endsWithNL;
        if (!hasNL) continue;
        std::wstring& l = g.lines[i];
        size_t vl = VLen(l);
        std::wstring n = l.substr(0, vl);
        if (toCrlf) n += L'\r';
        if (n != l) { l = n; changed = true; }
    }
    if (!changed) {
        g.crlfStyle = toCrlf;
        g.eolName = toCrlf ? L"CRLF" : L"LF";
        return;
    }
    g.crlfStyle = toCrlf;
    g.eolName = toCrlf ? L"CRLF" : L"LF";
    g.dirty = true;
    UpdateDirty();
    UpdateTitle();
    Refresh();
}

static void ShowEolMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"转换为 CRLF（Windows）");
    AppendMenuW(m, MF_STRING, 2, L"转换为 LF（Unix）");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g.showEol ? MF_CHECKED : 0), 3, L"显示行尾符号");
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                  pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) ConvertEol(true);
    else if (cmd == 2) ConvertEol(false);
    else if (cmd == 3) {
        g.showEol = !g.showEol;
        SaveSettings();
        InvalidateRect(g.hwnd, nullptr, FALSE);
    }
}

// -----------------------------------------------------------------------------
// 选区与编辑动作（全部通过 EditRange 记录撤销）
// -----------------------------------------------------------------------------
static void ClearSel() {
    g.hasSel = false;
    g.anchorL = g.curL;
    g.anchorC = g.curC;
}

static void GetSel(size_t& l1, size_t& c1, size_t& l2, size_t& c2) {
    if (!g.hasSel) { l1 = l2 = g.curL; c1 = c2 = g.curC; return; }
    if (g.anchorL < g.curL || (g.anchorL == g.curL && g.anchorC <= g.curC)) {
        l1 = g.anchorL; c1 = g.anchorC; l2 = g.curL; c2 = g.curC;
    } else {
        l1 = g.curL; c1 = g.curC; l2 = g.anchorL; c2 = g.anchorC;
    }
}

static inline bool HasSelection() {
    return g.hasSel && !(g.anchorL == g.curL && g.anchorC == g.curC);
}

static void InsertText(const std::wstring& s, bool typed) {
    if (s.empty()) return;
    size_t l1 = g.curL, c1 = g.curC, l2 = g.curL, c2 = g.curC;
    if (HasSelection()) GetSel(l1, c1, l2, c2);
    EditRange(l1, c1, l2, c2, s, typed && s.size() == 1);
}

static void KeyBackspace() {
    if (HasSelection()) {
        size_t l1, c1, l2, c2;
        GetSel(l1, c1, l2, c2);
        EditRange(l1, c1, l2, c2, L"", false);
        return;
    }
    if (g.curC > 0) {
        size_t c = g.curC - 1;
        if (c > 0 && IsLow(g.lines[g.curL][c]) && IsHigh(g.lines[g.curL][c - 1])) --c;
        EditRange(g.curL, c, g.curL, g.curC, L"", false);
    } else if (g.curL > 0) {
        size_t pl = VLen(g.lines[g.curL - 1]);
        EditRange(g.curL - 1, pl, g.curL, 0, L"", false);
    }
}

static void KeyDelete() {
    if (HasSelection()) {
        size_t l1, c1, l2, c2;
        GetSel(l1, c1, l2, c2);
        EditRange(l1, c1, l2, c2, L"", false);
        return;
    }
    size_t vl = VLen(g.lines[g.curL]);
    if (g.curC < vl) {
        size_t n = 1;
        if (IsHigh(g.lines[g.curL][g.curC]) && g.curC + 1 < vl &&
            IsLow(g.lines[g.curL][g.curC + 1])) n = 2;
        EditRange(g.curL, g.curC, g.curL, g.curC + n, L"", false);
    } else if (g.curL + 1 < g.lines.size()) {
        EditRange(g.curL, g.curC, g.curL + 1, 0, L"", false);
    }
}

static inline bool IsWordChar(wchar_t c) {
    if (c < 32) return false;
    if (c < 128) return iswalnum((wint_t)c) != 0 || c == L'_';
    return !iswspace((wint_t)c);
}

static void KeyBackspaceWord() {
    if (HasSelection()) {
        size_t l1, c1, l2, c2;
        GetSel(l1, c1, l2, c2);
        EditRange(l1, c1, l2, c2, L"", false);
        return;
    }
    if (g.curC == 0) { KeyBackspace(); return; }
    const std::wstring& ln = g.lines[g.curL];
    size_t c = g.curC;
    while (c > 0 && iswspace((wint_t)ln[c - 1])) --c;
    if (c > 0 && IsWordChar(ln[c - 1]))
        while (c > 0 && IsWordChar(ln[c - 1])) --c;
    else if (c > 0) --c;
    EditRange(g.curL, c, g.curL, g.curC, L"", false);
}

static void KeyDeleteWord() {
    if (HasSelection()) {
        size_t l1, c1, l2, c2;
        GetSel(l1, c1, l2, c2);
        EditRange(l1, c1, l2, c2, L"", false);
        return;
    }
    size_t vl = VLen(g.lines[g.curL]);
    if (g.curC >= vl) { KeyDelete(); return; }
    const std::wstring& ln = g.lines[g.curL];
    size_t c = g.curC;
    if (IsWordChar(ln[c]))
        while (c < vl && IsWordChar(ln[c])) ++c;
    else
        while (c < vl && !IsWordChar(ln[c])) ++c;
    EditRange(g.curL, g.curC, g.curL, c, L"", false);
}

static void CopySel(bool cut) {
    if (!HasSelection()) return;
    size_t l1, c1, l2, c2;
    GetSel(l1, c1, l2, c2);
    std::wstring out;
    for (size_t i = l1; i <= l2; ++i) {
        size_t a = (i == l1 ? c1 : 0), b = (i == l2 ? c2 : VLen(g.lines[i]));
        out.append(g.lines[i].data() + a, b - a);
        if (i < l2) out += L"\r\n";
    }
    if (OpenClipboard(g.hwnd)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (out.size() + 1) * sizeof(wchar_t));
        if (h) {
            wchar_t* p = (wchar_t*)GlobalLock(h);
            if (p) {
                memcpy(p, out.c_str(), (out.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            }
        }
        CloseClipboard();
    }
    if (cut) EditRange(l1, c1, l2, c2, L"", false);
}

static void Paste() {
    std::wstring t;
    if (OpenClipboard(g.hwnd)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const wchar_t* p = (const wchar_t*)GlobalLock(h);
            if (p) {
                std::wstring s(p);
                t.reserve(s.size());
                for (size_t i = 0; i < s.size(); ++i) {
                    if (s[i] == L'\r' || s[i] == 0) continue;
                    t += s[i];
                }
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
    }
    if (!t.empty()) InsertText(t, false);
}

static void SelectAll() {
    g.anchorL = 0; g.anchorC = 0;
    g.curL = g.lines.size() - 1;
    g.curC = VLen(g.lines[g.curL]);
    g.hasSel = true;
    AfterMove(true);
}

static void MoveHoriz(int dir, bool ext) {
    if (ext && !g.hasSel) { g.anchorL = g.curL; g.anchorC = g.curC; }
    std::wstring& L = g.lines[g.curL];
    size_t vl = VLen(L);
    if (dir < 0) {
        if (g.curC > 0) {
            --g.curC;
            if (g.curC > 0 && IsLow(L[g.curC]) && IsHigh(L[g.curC - 1])) --g.curC;
        } else if (g.curL > 0) {
            --g.curL; g.curC = VLen(g.lines[g.curL]);
        }
    } else {
        if (g.curC < vl) {
            ++g.curC;
            if (g.curC < vl && IsLow(L[g.curC]) && IsHigh(L[g.curC - 1])) ++g.curC;
        } else if (g.curL + 1 < g.lines.size()) {
            ++g.curL; g.curC = 0;
        }
    }
    g.hasSel = ext;
    AfterMove(true);
}

static void MoveHomeEnd(bool end, bool ctrl, bool ext) {
    if (ext && !g.hasSel) { g.anchorL = g.curL; g.anchorC = g.curC; }
    if (ctrl) {
        g.curL = end ? g.lines.size() - 1 : 0;
        g.curC = end ? VLen(g.lines[g.curL]) : 0;
    } else if (!end) {
        size_t first = 0;
        size_t vl = VLen(g.lines[g.curL]);
        while (first < vl && (g.lines[g.curL][first] == L' ' || g.lines[g.curL][first] == L'\t'))
            ++first;
        g.curC = (g.curC == first) ? 0 : first;
    } else {
        g.curC = VLen(g.lines[g.curL]);
    }
    g.hasSel = ext;
    AfterMove(true);
}

static void PageMove(int dir, bool ext) {
    MoveVert(dir * VisLines(), ext);
}

// -----------------------------------------------------------------------------
// 颜色
// -----------------------------------------------------------------------------
static void InitColors() {
    if (g.dark) {
        g.cBg = RGB(30, 30, 30);       g.cText = RGB(212, 212, 212);
        g.cGutter = RGB(30, 30, 30);   g.cGutterText = RGB(105, 110, 118);
        g.cBar = RGB(43, 43, 43);      g.cBarText = RGB(228, 228, 230);
        g.cSep = RGB(62, 62, 62);      g.cStatusText = RGB(150, 154, 162);
        g.cBtn = RGB(51, 51, 51);      g.cBtnHover = RGB(64, 64, 64);
        g.cBtnDown = RGB(76, 76, 76);  g.cBtnBorder = RGB(78, 78, 78);
        g.cSel = RGB(38, 79, 120);     g.cSelText = RGB(255, 255, 255);
        g.cCurLine = RGB(37, 37, 38);  g.cCaret = RGB(230, 230, 235);
        g.cField = RGB(37, 37, 38);    g.cFieldBorder = RGB(80, 80, 84);
        g.cFieldText = RGB(222, 222, 226); g.cPh = RGB(120, 124, 132);
        g.cFind = RGB(96, 80, 26);     g.cFindText = RGB(255, 255, 255);
        g.cAccent = RGB(84, 148, 255);
    } else {
        g.cBg = RGB(255, 255, 255);    g.cText = RGB(28, 30, 34);
        g.cGutter = RGB(250, 250, 252);g.cGutterText = RGB(168, 172, 181);
        g.cBar = RGB(247, 247, 249);   g.cBarText = RGB(42, 44, 50);
        g.cSep = RGB(229, 230, 234);   g.cStatusText = RGB(118, 122, 132);
        g.cBtn = RGB(255, 255, 255);   g.cBtnHover = RGB(238, 240, 245);
        g.cBtnDown = RGB(226, 229, 236);g.cBtnBorder = RGB(222, 224, 230);
        g.cSel = RGB(198, 220, 255);   g.cSelText = RGB(28, 30, 34);
        g.cCurLine = RGB(248, 250, 255);g.cCaret = RGB(40, 100, 220);
        g.cField = RGB(255, 255, 255); g.cFieldBorder = RGB(210, 213, 220);
        g.cFieldText = RGB(28, 30, 34); g.cPh = RGB(162, 166, 175);
        g.cFind = RGB(255, 226, 138);  g.cFindText = RGB(28, 30, 34);
        g.cAccent = RGB(40, 100, 220);
    }
}

static bool SystemDark() {
    HKEY hk = nullptr;
    DWORD val = 1, sz = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, nullptr, (LPBYTE)&val, &sz);
        RegCloseKey(hk);
    }
    return val == 0;
}

// -----------------------------------------------------------------------------
// 工具栏 / 状态栏布局
// -----------------------------------------------------------------------------
static RECT BtnRect(int id, int cw) {
    const int w = MulDiv(72, g.dpi, 96), h = MulDiv(30, g.dpi, 96);
    const int gap = MulDiv(8, g.dpi, 96), right = MulDiv(14, g.dpi, 96);
    int x = cw - right - w - (5 - id) * (gap + w);
    int y = (g.toolbarH - h) / 2;
    return RECT{ x, y, x + w, y + h };
}

static RECT FileNameRect() {
    RECT b = BtnRect(1, g.clientW);
    LONG left = MulDiv(18, g.dpi, 96);
    LONG right = std::max(left, b.left - MulDiv(16, g.dpi, 96));
    return RECT{ left, 0, right, g.toolbarH };
}

static void DrawButton(HDC h, int id, const wchar_t* text, bool active) {
    RECT r = BtnRect(id, g.clientW);
    int state = (g.downBtn == id) ? 2 : (g.hoverBtn == id ? 1 : 0);
    COLORREF fill = active ? g.cAccent
                    : (state == 2 ? g.cBtnDown : (state == 1 ? g.cBtnHover : g.cBtn));
    COLORREF border = active ? g.cAccent : g.cBtnBorder;
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(h, br), op = SelectObject(h, pen);
    RoundRect(h, r.left, r.top, r.right, r.bottom,
              MulDiv(8, g.dpi, 96), MulDiv(8, g.dpi, 96));
    SelectObject(h, ob);
    SelectObject(h, op);
    DeleteObject(br);
    DeleteObject(pen);
    SetTextColor(h, active ? RGB(255, 255, 255) : g.cBarText);
    HGDIOBJ of = SelectObject(h, g.hUI);
    DrawTextW(h, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(h, of);
}

static void DrawToolbar(HDC h) {
    Fill(h, 0, 0, g.clientW, g.toolbarH, g.cBar);
    Line(h, 0, g.toolbarH - 1, g.clientW, g.toolbarH - 1, g.cSep);

    if (g.hRename && IsWindowVisible(g.hRename)) {
        // 内联重命名时文件名由编辑框显示
    } else {
        std::wstring name = g.path.empty() ? L"未命名" : FileName(g.path);
        RECT t = FileNameRect();
        RECT tr = { t.left + MulDiv(8, g.dpi, 96), 0,
                    t.right - MulDiv(6, g.dpi, 96), g.toolbarH };
        SetTextColor(h, g.cBarText);
        HGDIOBJ of = SelectObject(h, g.hUIBold);
        DrawTextW(h, name.c_str(), -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(h, of);
        if (g.hoverName) {
            SIZE sz = { 0, 0 };
            HGDIOBJ of2 = SelectObject(h, g.hUIBold);
            GetTextExtentPoint32W(h, name.c_str(), (int)name.size(), &sz);
            SelectObject(h, of2);
            Line(h, tr.left, g.toolbarH - MulDiv(9, g.dpi, 96),
                 tr.left + sz.cx, g.toolbarH - MulDiv(9, g.dpi, 96), g.cAccent);
        }
    }

    DrawButton(h, 1, L"打开", false);
    DrawButton(h, 2, L"保存", false);
    DrawButton(h, 3, L"另存为", false);
    DrawButton(h, 4, L"置顶", g.topmost);
    DrawButton(h, 5, L"换行", g.wrap);
}

// 状态栏分段
struct StatusLayout { RECT pos, lines, enc, eol, dirty; };

static int TW(HDC h, const std::wstring& s) {
    SIZE z = { 0, 0 };
    GetTextExtentPoint32W(h, s.c_str(), (int)s.size(), &z);
    return z.cx;
}

static StatusLayout StatusRects(HDC hdc, int y) {
    StatusLayout L = {};
    HGDIOBJ of = SelectObject(hdc, g.hUI);
    std::wstring sPos = L"行 " + std::to_wstring(g.curL + 1) + L", 列 " + std::to_wstring(g.curC + 1);
    std::wstring sLines = std::to_wstring(g.lines.size()) + L" 行";
    std::wstring sEnc = g.enc.Name();
    std::wstring sEol = g.eolName;
    std::wstring sDirty = g.dirty ? L"已修改" : L"未修改";
    if (!g.flash.empty() && GetTickCount64() < g.flashUntil) sDirty = g.flash;
    int x = MulDiv(14, g.dpi, 96);
    int pad = MulDiv(16, g.dpi, 96);
    auto place = [&](const std::wstring& s, RECT& r) {
        int w = TW(hdc, s);
        r = { x, y, x + w, y + g.statusH };
        x += w + pad;
    };
    place(sPos, L.pos);
    place(sLines, L.lines);
    place(sEnc, L.enc);
    place(sEol, L.eol);
    place(sDirty, L.dirty);
    SelectObject(hdc, of);
    return L;
}

static void DrawStatus(HDC h) {
    int y = g.clientH - g.statusH;
    Fill(h, 0, y, g.clientW, g.clientH, g.cBar);
    Line(h, 0, y, g.clientW, y, g.cSep);
    StatusLayout L = StatusRects(h, y);
    HGDIOBJ of = SelectObject(h, g.hUI);
    SetTextColor(h, g.cStatusText);
    DrawTextW(h, (L"行 " + std::to_wstring(g.curL + 1) + L", 列 " + std::to_wstring(g.curC + 1)).c_str(),
              -1, &L.pos, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(h, (std::to_wstring(g.lines.size()) + L" 行").c_str(), -1, &L.lines,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(h, g.enc.Name(), -1, &L.enc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(h, g.eolName.c_str(), -1, &L.eol, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    bool flashing = !g.flash.empty() && GetTickCount64() < g.flashUntil;
    SetTextColor(h, flashing ? g.cAccent : (g.dirty ? g.cAccent : g.cStatusText));
    std::wstring sDirty = flashing ? g.flash : (g.dirty ? L"已修改" : L"未修改");
    DrawTextW(h, sDirty.c_str(), -1, &L.dirty, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 分段竖线
    const RECT* seps[] = { &L.pos, &L.lines, &L.enc, &L.eol };
    for (int k = 0; k < 4; ++k) {
        int sx = (k == 3 ? L.dirty.left : seps[k + 1]->left) - MulDiv(8, g.dpi, 96);
        Line(h, sx, y + g.statusH / 3, sx, y + g.statusH * 2 / 3, g.cSep);
    }

    // 右侧文件大小
    std::wstring right = FmtSize(g.fileSize);
    SetTextColor(h, g.cStatusText);
    RECT rr = { g.clientW - MulDiv(160, g.dpi, 96), y, g.clientW - MulDiv(14, g.dpi, 96), g.clientH };
    DrawTextW(h, right.c_str(), -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(h, of);
}

// 输入框（查找栏）
static void DrawField(HDC h, const RECT& r, const std::wstring& text, const std::wstring& ph,
                      bool focused, bool caretVisible) {
    HBRUSH fb = CreateSolidBrush(g.cField);
    HPEN fp = CreatePen(PS_SOLID, 1, focused ? g.cAccent : g.cFieldBorder);
    HGDIOBJ ob = SelectObject(h, fb), op = SelectObject(h, fp);
    RoundRect(h, r.left, r.top, r.right, r.bottom,
              MulDiv(8, g.dpi, 96), MulDiv(8, g.dpi, 96));
    SelectObject(h, ob);
    SelectObject(h, op);
    DeleteObject(fb);
    DeleteObject(fp);

    HGDIOBJ of = SelectObject(h, g.hUI);
    RECT tr = { r.left + MulDiv(10, g.dpi, 96), r.top,
                r.right - MulDiv(8, g.dpi, 96), r.bottom };
    const bool empty = text.empty();
    const std::wstring shown = empty ? ph : text;
    SetTextColor(h, empty ? g.cPh : g.cFieldText);
    DrawTextW(h, shown.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (focused && caretVisible) {
        SIZE ts = { 0, 0 };
        GetTextExtentPoint32W(h, shown.c_str(), (int)shown.size(), &ts);
        int cx = tr.left + ts.cx + 1;
        if (cx > tr.right - 2) cx = tr.right - 2;
        Fill(h, cx, r.top + MulDiv(5, g.dpi, 96),
             cx + std::max(1, MulDiv(2, g.dpi, 96)),
             r.bottom - MulDiv(5, g.dpi, 96), g.cAccent);
    }
    SelectObject(h, of);
}

static void DrawFindBar(HDC h) {
    const int top = g.toolbarH;
    Fill(h, 0, top, g.clientW, top + g.findH, g.cBar);
    Line(h, 0, top + g.findH - 1, g.clientW, top + g.findH - 1, g.cSep);
    FindLayout L = FindRects();

    HGDIOBJ ofUI = SelectObject(h, g.hUI);
    SetBkMode(h, TRANSPARENT);

    DrawField(h, L.field, g.find.q, L"查找…", g.find.activeField == 0, g.caretOn);
    if (g.find.replaceMode)
        DrawField(h, L.repField, g.find.rep, L"替换为…", g.find.activeField == 1, g.caretOn);

    // 命中计数
    std::wstring cnt;
    if (!g.find.q.empty()) {
        if (g.find.dirtyHits) cnt = L"…";
        else if (g.find.hits.empty()) cnt = L"无结果";
        else {
            int cur = (g.find.current < 0) ? 1 : g.find.current + 1;
            cnt = std::to_wstring(cur) + L"/" + std::to_wstring(g.find.hits.size());
            if (g.find.truncated) cnt += L"+";
        }
    }
    RECT cr = { L.field.right + MulDiv(8, g.dpi, 96), L.field.top,
                L.up.left - MulDiv(6, g.dpi, 96), L.field.bottom };
    SetTextColor(h, g.cStatusText);
    DrawTextW(h, cnt.c_str(), -1, &cr,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    auto btn = [&](const RECT& r, int id, const wchar_t* text, bool accent, bool bold) {
        if (g.hoverFindBtn == id) {
            HBRUSH hb = CreateSolidBrush(g.cBtnHover);
            HPEN hp = CreatePen(PS_SOLID, 1, g.cBtnBorder);
            HGDIOBJ o = SelectObject(h, hb);
            HGDIOBJ p = SelectObject(h, hp);
            RoundRect(h, r.left, r.top, r.right, r.bottom,
                      MulDiv(6, g.dpi, 96), MulDiv(6, g.dpi, 96));
            SelectObject(h, o);
            SelectObject(h, p);
            DeleteObject(hb);
            DeleteObject(hp);
        }
        SetTextColor(h, accent ? g.cAccent : g.cBarText);
        HGDIOBJ f = SelectObject(h, bold ? g.hUIBold : g.hUI);
        RECT rr = r;
        DrawTextW(h, text, -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(h, f);
    };
    btn(L.up, 1, L"▲", false, false);
    btn(L.down, 2, L"▼", false, false);
    btn(L.aa, 3, L"Aa", g.find.caseSensitive, g.find.caseSensitive);
    btn(L.close, 4, L"×", false, false);
    if (g.find.replaceMode) {
        btn(L.repl, 5, L"替换", false, false);
        btn(L.replAll, 6, L"全部替换", false, false);
        btn(L.fromStart, 7, L"从头", g.find.fromStart, g.find.fromStart);
    }
    SelectObject(h, ofUI);
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    g.clientW = rc.right;
    g.clientH = rc.bottom;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);

    Fill(mem, 0, 0, rc.right, rc.bottom, g.cBg);
    int textTop = TextTop();
    int textBottom = std::max<int>(textTop, (int)(rc.bottom - g.statusH));
    Fill(mem, 0, textTop, g.gutterW, textBottom, g.cGutter);
    Line(mem, g.gutterW - 1, textTop, g.gutterW - 1, textBottom, g.cSep);

    HGDIOBJ of = SelectObject(mem, g.hMono);

    size_t l1 = 0, c1 = 0, l2 = 0, c2 = 0;
    const bool hasSel = HasSelection();
    if (hasSel) GetSel(l1, c1, l2, c2);
    const bool findOn = g.find.open && !g.find.q.empty();

    SaveDC(mem);
    IntersectClipRect(mem, g.gutterW, textTop, rc.right, textBottom);

    std::vector<int> dx;
    std::vector<size_t> br;
    std::vector<size_t> fmatch;
    std::vector<std::pair<size_t, int>> gutterNums;
    int y = textTop;
    int caretX = 0, caretY = 0;
    bool caretDrawn = false;

    for (size_t i = g.topL; i < g.lines.size(); ++i) {
        std::wstring d = Disp(g.lines[i]);
        ComputeDx(mem, d, dx);
        br.clear();
        br.push_back(0);
        if (g.wrap && d.size() > 1) {
            const int fit = FitWidth();
            size_t start = 0;
            for (size_t c = 1; c < d.size(); ++c) {
                if (dx[c + 1] - dx[start] > fit) { br.push_back(c); start = c; }
            }
        }
        const size_t dlen = d.size();
        if (!g.wrap && !dx.empty() && dx.back() > g.maxLinePx) g.maxLinePx = dx.back();

        // 本行所有查找命中
        fmatch.clear();
        if (findOn) {
            const std::wstring& q = g.find.q;
            if (dlen >= q.size()) {
                for (size_t c = 0; c + q.size() <= dlen; ++c) {
                    if (!MatchAt(g.lines[i], c, q)) continue;
                    fmatch.push_back(c);
                    c += q.size() - 1;
                }
            }
        }

        for (size_t k = 0; k < br.size(); ++k) {
            if (y >= textBottom) break;
            const size_t segStart = br[k];
            const size_t segEnd = (k + 1 < br.size()) ? br[k + 1] : dlen;

            if (i == g.curL)
                Fill(mem, 0, y, rc.right, y + g.lineH, g.cCurLine);

            // 选区
            if (hasSel && i >= l1 && i <= l2) {
                size_t a = (i == l1 ? c1 : 0), b = (i == l2 ? c2 : dlen);
                a = std::min(a, dlen);
                b = std::min(b, dlen);
                size_t xa = std::max(a, segStart), xb = std::min(b, segEnd);
                if (xb > xa) {
                    RECT sr = { g.gutterW + dx[xa] - dx[segStart] - g.xOffset, y,
                                g.gutterW + dx[xb] - dx[segStart] - g.xOffset, y + g.lineH };
                    HBRUSH sb = CreateSolidBrush(g.cSel);
                    FillRect(mem, &sr, sb);
                    DeleteObject(sb);
                }
            }

            // 查找高亮（当前命中由选区标示）
            if (findOn) {
                int curIdx = g.find.dirtyHits ? -1 : g.find.current;
                for (size_t m : fmatch) {
                    if (m < segStart || m >= segEnd) continue;
                    bool isCur = curIdx >= 0 && (size_t)curIdx < g.find.hits.size() &&
                                 g.find.hits[curIdx].first == i &&
                                 g.find.hits[curIdx].second == m;
                    if (isCur) continue;
                    size_t me = std::min(m + g.find.q.size(), dlen);
                    RECT fr = { g.gutterW + dx[m] - dx[segStart] - g.xOffset, y,
                                g.gutterW + dx[me] - dx[segStart] - g.xOffset, y + g.lineH };
                    HBRUSH hb = CreateSolidBrush(g.cFind);
                    FillRect(mem, &fr, hb);
                    DeleteObject(hb);
                }
            }

            // 文本
            if (segEnd > segStart) {
                SetTextColor(mem, g.cText);
                ExtTextOutW(mem, g.gutterW - g.xOffset,
                            y + (g.lineH - g.fontPx) / 2, 0, nullptr,
                            d.c_str() + segStart, (UINT)(segEnd - segStart), nullptr);
            }

            // 行尾符号
            if (g.showEol && segEnd == dlen && dlen < dx.size()) {
                int ex = g.gutterW + dx[dlen] - dx[segStart] - g.xOffset + 3;
                SetTextColor(mem, g.cGutterText);
                HGDIOBJ of2 = SelectObject(mem, g.hUI);
                ExtTextOutW(mem, ex, y + (g.lineH - MulDiv(13, g.dpi, 96)) / 2, 0, nullptr,
                            L"\u21B5", 1, nullptr);
                SelectObject(mem, of2);
                SetTextColor(mem, g.cText);
            }

            // 光标
            if (i == g.curL && k == (size_t)CaretSeg(br, g.curC) && !caretDrawn) {
                size_t cc = std::min(g.curC, dlen);
                caretX = g.gutterW + dx[cc] - dx[segStart] - g.xOffset;
                caretY = y;
                caretDrawn = true;
            }

            // 行号（先收集，稍后在 gutter 独立裁剪区绘制）
            if (k == 0) gutterNums.push_back({ i, y });
            y += g.lineH;
        }
        if (y >= textBottom) break;
    }

    // 自绘闪烁光标
    if (caretDrawn && !g.find.open && g.hasFocus && g.caretOn) {
        Fill(mem, caretX, caretY + MulDiv(2, g.dpi, 96),
             caretX + std::max(1, MulDiv(2, g.dpi, 96)),
             caretY + g.lineH - MulDiv(3, g.dpi, 96), g.cCaret);
    }
    RestoreDC(mem, -1);

    // 行号：独立裁剪到 gutter，避免被正文裁剪区切掉
    if (!gutterNums.empty()) {
        SaveDC(mem);
        IntersectClipRect(mem, 0, textTop, g.gutterW, textBottom);
        HGDIOBJ ofn = SelectObject(mem, g.hUI);
        SetTextColor(mem, g.cGutterText);
        for (size_t q = 0; q < gutterNums.size(); ++q) {
            wchar_t num[32];
            swprintf(num, 32, L"%llu", (unsigned long long)(gutterNums[q].first + 1));
            RECT nr = { 0, gutterNums[q].second, g.gutterW - MulDiv(10, g.dpi, 96),
                        gutterNums[q].second + g.lineH };
            DrawTextW(mem, num, -1, &nr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(mem, ofn);
        RestoreDC(mem, -1);
    }

    SelectObject(mem, of);
    DrawToolbar(mem);
    if (g.find.open) DrawFindBar(mem);
    DrawStatus(mem);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// -----------------------------------------------------------------------------
// 鼠标定位
// -----------------------------------------------------------------------------
static void MouseToCaret(int x, int y, bool ext) {
    if (g.lines.empty()) return;
    HDC hdc = MeasureDC();
    size_t l = g.lines.size() - 1;
    size_t col = 0;

    if (g.wrap) {
        int yy = TextTop();
        size_t i = g.topL;
        for (; i < g.lines.size(); ++i) {
            int rows = LineRows(hdc, i);
            if (y < yy + rows * g.lineH) break;
            yy += rows * g.lineH;
        }
        if (i >= g.lines.size()) {
            l = g.lines.size() - 1;
            col = VLen(g.lines[l]);
        } else {
            l = i;
            std::vector<int> dx;
            std::vector<size_t> br;
            LayoutLine(hdc, i, dx, br);
            int rowInLine = (y - yy) / g.lineH;
            if (rowInLine < 0) rowInLine = 0;
            if (rowInLine >= (int)br.size()) rowInLine = (int)br.size() - 1;
            size_t segStart = br[rowInLine];
            size_t segEnd = (rowInLine + 1 < (int)br.size()) ? br[rowInLine + 1] : dx.size() - 1;
            std::wstring d = Disp(g.lines[i]);
            size_t c = ColFromX(d, dx, x - g.gutterW + g.xOffset + dx[segStart]);
            col = std::min(std::max(c, segStart), std::min(segEnd, d.size()));
        }
    } else {
        int row = (y - TextTop()) / g.lineH;
        if (row < 0) row = 0;
        l = g.topL + (size_t)row;
        if (l >= g.lines.size()) l = g.lines.size() - 1;
        std::wstring d = Disp(g.lines[l]);
        std::vector<int> dx;
        ComputeDx(hdc, d, dx);
        col = ColFromX(d, dx, x - g.gutterW + g.xOffset);
    }

    if (ext && !g.hasSel) { g.anchorL = g.curL; g.anchorC = g.curC; }
    g.curL = l;
    g.curC = col;
    g.hasSel = ext;
    g.desiredX = -1;
    Refresh();
}

static void SelectWordAt(size_t l, size_t c) {
    const std::wstring& ln = g.lines[l];
    size_t vl = VLen(ln);
    size_t a = std::min(c, vl), b = a;
    if (a < vl && IsWordChar(ln[a])) {
        while (a > 0 && IsWordChar(ln[a - 1])) --a;
        while (b < vl && IsWordChar(ln[b])) ++b;
    } else {
        a = 0; b = vl;
    }
    g.anchorL = l; g.anchorC = a;
    g.curL = l; g.curC = b;
    g.hasSel = b > a;
    AfterMove(true);
}

static int HitButton(int x, int y) {
    for (int id = 1; id <= 5; ++id) {
        RECT r = BtnRect(id, g.clientW);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return id;
    }
    return 0;
}

// -----------------------------------------------------------------------------
// 置顶 / 自动换行 / 缩放
// -----------------------------------------------------------------------------
static void ToggleTopmost() {
    g.topmost = !g.topmost;
    SetWindowPos(g.hwnd, g.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
    SaveSettings();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void ToggleWrap() {
    g.wrap = !g.wrap;
    if (g.wrap) g.xOffset = 0;
    SaveSettings();
    EnsureCaretVisible();
    UpdateScroll();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void MakeMonoFont() {
    if (g.hMono) DeleteObject(g.hMono);
    g.hMono = CreateFontW(-g.fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HDC hdc = MeasureDC();
    SelectObject(hdc, g.hMono);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(hdc, L"M", 1, &sz);
    g.charW = sz.cx ? sz.cx : 9;
    g.lineH = g.fontPx + MulDiv(6, g.dpi, 96);
}

static void ZoomFont(int dir) {
    int base = g.dpi ? MulDiv(g.fontPx, 96, g.dpi) : g.fontPx;
    base = std::clamp(base + dir, 8, 40);
    int px = MulDiv(base, g.dpi, 96);
    if (px == g.fontPx) return;
    g.fontPx = px;
    MakeMonoFont();
    SaveSettings();
    EnsureCaretVisible();
    UpdateScroll();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// -----------------------------------------------------------------------------
// 内联重命名
// -----------------------------------------------------------------------------
static WNDPROC g_oldEditProc = nullptr;
static bool g_renameBusy = false;

static std::wstring TrimText(std::wstring s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}

static std::wstring GetRenameText() {
    std::wstring s;
    if (!g.hRename) return s;
    int n = GetWindowTextLengthW(g.hRename);
    if (n <= 0) return s;
    s.resize((size_t)n + 1);
    GetWindowTextW(g.hRename, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

static void HideRename() {
    if (g.hRename && IsWindowVisible(g.hRename)) ShowWindow(g.hRename, SW_HIDE);
}

static void CancelRename() {
    g_renameBusy = true;
    HideRename();
    g_renameBusy = false;
    SetFocus(g.hwnd);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void CommitRename() {
    if (!g.hRename || g_renameBusy) return;
    g_renameBusy = true;
    std::wstring name = TrimText(GetRenameText());
    HideRename();
    g_renameBusy = false;
    SetFocus(g.hwnd);
    InvalidateRect(g.hwnd, nullptr, FALSE);
    if (name.empty()) return;

    if (g.path.empty()) {
        std::wstring out;
        if (PickSave(name, out)) {
            g.path = out;
            SaveFile(false);
        }
        return;
    }
    std::wstring old = g.path;
    std::wstring dir = DirName(old);
    std::wstring np = dir.empty() ? name : dir + L"\\" + name;
    if (np == old) return;
    if (GetFileAttributesW(ExtPath(np).c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (MessageBoxW(g.hwnd, L"同名文件已存在，继续将覆盖它，是否继续？", L"Bare",
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
    }
    if (!MoveFileExW(ExtPath(old).c_str(), ExtPath(np).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MessageBoxW(g.hwnd, L"重命名失败：文件可能被占用或没有权限。", L"Bare",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    g.path = np;
    UpdateStamp();
    UpdateTitle();
    Refresh();
}

static LRESULT CALLBACK RenameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_KEYDOWN:
            if (wp == VK_RETURN)   { CommitRename(); return 0; }
            if (wp == VK_ESCAPE)   { CancelRename(); return 0; }
            break;
        case WM_KILLFOCUS:
            if (!g_renameBusy && g.hRename && IsWindowVisible(g.hRename)) CommitRename();
            break;
        case WM_CHAR:
            if (wp == VK_RETURN || wp == VK_ESCAPE) return 0;
            break;
    }
    return CallWindowProcW(g_oldEditProc, h, msg, wp, lp);
}

static void StartRename() {
    if (!g.hRename) {
        g.hRename = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                    0, 0, 10, 10, g.hwnd, (HMENU)1001, g.hinst, nullptr);
        if (!g.hRename) return;
        SendMessageW(g.hRename, WM_SETFONT, (WPARAM)g.hUI, TRUE);
        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(g.hRename, GWLP_WNDPROC,
                                                   (LONG_PTR)RenameProc);
    }
    std::wstring name = g.path.empty() ? std::wstring(L"未命名") : FileName(g.path);
    SetWindowTextW(g.hRename, name.c_str());
    RECT r = FileNameRect();
    int w = std::max(MulDiv(120, g.dpi, 96), (int)(r.right - r.left) - MulDiv(6, g.dpi, 96));
    int h = MulDiv(26, g.dpi, 96);
    SetWindowPos(g.hRename, HWND_TOP, r.left + MulDiv(2, g.dpi, 96),
                 (g.toolbarH - h) / 2, w, h, SWP_SHOWWINDOW);
    SendMessageW(g.hRename, EM_SETSEL, 0, -1);
    SetFocus(g.hRename);
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

// -----------------------------------------------------------------------------
// 右键菜单
// -----------------------------------------------------------------------------
static void ShowContextMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g.undo.empty() ? MF_GRAYED : 0), 1, L"撤销\tCtrl+Z");
    AppendMenuW(m, MF_STRING | (g.redo.empty() ? MF_GRAYED : 0), 2, L"重做\tCtrl+Y");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (HasSelection() ? 0 : MF_GRAYED), 3, L"剪切\tCtrl+X");
    AppendMenuW(m, MF_STRING | (HasSelection() ? 0 : MF_GRAYED), 4, L"复制\tCtrl+C");
    AppendMenuW(m, MF_STRING, 5, L"粘贴\tCtrl+V");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 6, L"全选\tCtrl+A");
    AppendMenuW(m, MF_STRING, 7, L"查找\tCtrl+F");
    AppendMenuW(m, MF_STRING, 8, L"查找并替换\tCtrl+H");
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                  pt.x, pt.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    switch (cmd) {
        case 1: UndoRedo(false); break;
        case 2: UndoRedo(true); break;
        case 3: CopySel(true); break;
        case 4: CopySel(false); break;
        case 5: Paste(); break;
        case 6: SelectAll(); break;
        case 7: OpenFind(false); break;
        case 8: OpenFind(true); break;
        default: break;
    }
}

// -----------------------------------------------------------------------------
// 外部文件修改监测
// -----------------------------------------------------------------------------
static void CheckExternalChange() {
    if (g.path.empty() || !g.hasStamp || g.watching) return;
    FILETIME ft;
    unsigned long long sz = 0;
    if (!GetStamp(g.path, ft, sz)) return;
    if (CompareFileTime(&ft, &g.stamp) == 0 && sz == g.fileSize) return;

    g.watching = true;
    std::wstring msg = L"文件「" + FileName(g.path) +
                       L"」已被其他程序修改。\n\n是(Y)：重新载入最新内容\n否(N)：保持当前内容";
    int r = MessageBoxW(g.hwnd, msg.c_str(), L"Bare · 文件已变更",
                        MB_YESNO | MB_ICONINFORMATION);
    // 注意：MessageBox 会跑消息循环，期间 WM_TIMER 仍会进来；g.watching 必须保持到
    // 整个处理结束，否则会重入弹出第二个「文件已变更」对话框。
    if (r == IDYES) {
        if (g.dirty) {
            std::wstring warn = L"当前内容有未保存的修改，重新载入会丢失这些修改。\n\n是否继续重新载入？";
            if (MessageBoxW(g.hwnd, warn.c_str(), L"Bare", MB_YESNO | MB_ICONWARNING) != IDYES) {
                g.stamp = ft;
                g.fileSize = sz;
                g.watching = false;
                return;
            }
        }
        g.watching = false;
        LoadFile(g.path, -1, true);
    } else {
        g.stamp = ft;
        g.fileSize = sz;
        g.watching = false;
    }
}

static bool WantTextArea(int y) {
    return y >= TextTop() && y < g.clientH - g.statusH;
}

// -----------------------------------------------------------------------------
// 窗口过程
// -----------------------------------------------------------------------------
static size_t sSavedTop = 0;
static int sSavedX = 0;

static void CaptureWindowRect() {
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (GetWindowPlacement(g.hwnd, &wp)) {
        g.winMax = (wp.showCmd == SW_SHOWMAXIMIZED);
        if (g.winMax) {
            g.winRect = wp.rcNormalPosition;
        } else {
            RECT r = { 0, 0, 0, 0 };
            if (GetWindowRect(g.hwnd, &r)) g.winRect = r;
        }
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            g.dark = SystemDark();
            InitColors();

            HDC sdc = GetDC(hwnd);
            g.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
            if (g.dpi < 72) g.dpi = 96;
            ReleaseDC(hwnd, sdc);
            g.fontPx   = MulDiv(g.fontPx, g.dpi, 96);
            g.lineH    = g.fontPx + MulDiv(6, g.dpi, 96);
            g.gutterW  = MulDiv(56, g.dpi, 96);
            g.toolbarH = MulDiv(44, g.dpi, 96);
            g.statusH  = MulDiv(26, g.dpi, 96);
            g.findH    = MulDiv(40, g.dpi, 96);

            g.hUI = CreateFontW(-MulDiv(13, g.dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            g.hUIBold = CreateFontW(-MulDiv(14, g.dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            MakeMonoFont();

            BOOL dark = g.dark ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                                  &dark, sizeof(dark));

            g.lines.emplace_back();
            g.endsWithNL = false;
            g.crlfStyle = true;
            g.eolName = L"CRLF";
            DragAcceptFiles(hwnd, TRUE);
            SetTimer(hwnd, 1, 530, nullptr);
            SetTimer(hwnd, 3, 1200, nullptr);
            UpdateTitle();
            UpdateScroll();

            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv) {
                if (argc > 1 && argv[1][0]) LoadFile(argv[1]);
                LocalFree(argv);
            }
            return 0;
        }

        case WM_SIZE:
            g.clientW = LOWORD(lp);
            g.clientH = HIWORD(lp);
            EnsureCaretVisible();
            UpdateScroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            Paint(hwnd);
            return 0;

        case WM_SETFOCUS:
            g.hasFocus = true;
            g.caretOn = true;
            UpdateImePos();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_KILLFOCUS:
            g.hasFocus = false;
            if (g.selecting) { g.selecting = false; ReleaseCapture(); }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                sSavedTop = g.topL;
                sSavedX = g.xOffset;
            } else {
                g.topL = std::min(sSavedTop, (size_t)MaxTop());
                g.xOffset = g.wrap ? 0 : sSavedX;
                UpdateScroll();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_TIMER:
            if (wp == 1) {
                g.caretOn = !g.caretOn;
                if (!g.flash.empty() && GetTickCount64() >= g.flashUntil) {
                    g.flash.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            } else if (wp == 2) {
                KillTimer(hwnd, 2);
                if (g.find.open && g.find.dirtyHits) FindNav(true);
            } else if (wp == 3) {
                CheckExternalChange();
            }
            return 0;

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, g.cFieldText);
            SetBkColor(hdc, g.cField);
            static HBRUSH sEditBrush = nullptr;
            static COLORREF sEditColor = 0xFFFFFFFF;
            if (!sEditBrush || sEditColor != g.cField) {
                if (sEditBrush) DeleteObject(sEditBrush);
                sEditBrush = CreateSolidBrush(g.cField);
                sEditColor = g.cField;
            }
            return (LRESULT)sEditBrush;
        }

        case WM_IME_STARTCOMPOSITION:
            UpdateImePos();
            break;

        case WM_CHAR: {
            wchar_t ch = (wchar_t)wp;
            if (ch < 32) return 0;
            if (g.find.open) {
                if (g.find.activeField == 0) g.find.q.push_back(ch);
                else g.find.rep.push_back(ch);
                OnFindChanged();
                return 0;
            }
            if (IsHigh(ch)) { g.pendingHigh = ch; return 0; }
            std::wstring s;
            if (IsLow(ch) && g.pendingHigh) { s += g.pendingHigh; g.pendingHigh = 0; }
            else g.pendingHigh = 0;
            s += ch;
            InsertText(s, true);
            return 0;
        }

        case WM_KEYDOWN: {
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool navKey = (wp == VK_RETURN || wp == VK_BACK || wp == VK_ESCAPE ||
                           wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN ||
                           wp == VK_HOME || wp == VK_END || wp == VK_PRIOR || wp == VK_NEXT);
            if (navKey && ImeComposing()) break;

            if (g.find.open) {
                if (wp == VK_ESCAPE) { CloseFind(); return 0; }
                if (wp == VK_TAB && g.find.replaceMode) {
                    g.find.activeField = g.find.activeField ? 0 : 1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wp == VK_RETURN) {
                    if (ctrl && g.find.replaceMode) ReplaceAll();
                    else if (g.find.activeField == 1 && g.find.replaceMode) ReplaceCurrent();
                    else FindNav(!shift);
                    return 0;
                }
                if (wp == VK_F3) { FindNav(!shift); return 0; }
                if (wp == VK_BACK) {
                    std::wstring& t = (g.find.activeField == 0) ? g.find.q : g.find.rep;
                    if (!t.empty()) { t.pop_back(); OnFindChanged(); }
                    return 0;
                }
                if (ctrl) {
                    switch (wp) {
                        case L'Z': UndoRedo(shift); return 0;
                        case L'Y': UndoRedo(true); return 0;
                        case L'V': PasteToField(); return 0;
                        case L'S': if (shift) SaveFile(true); else SaveFile(false); return 0;
                        case L'O': OpenDialog(); return 0;
                        case L'N': NewFile(); return 0;
                        case L'H':
                            g.find.replaceMode = !g.find.replaceMode;
                            if (!g.find.replaceMode) g.find.activeField = 0;
                            SyncFindHeight();
                            UpdateScroll();
                            EnsureCaretVisible();
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        default: break;
                    }
                }
                return 0;
            }

            if (ctrl) {
                switch (wp) {
                    case L'Z': UndoRedo(shift); return 0;
                    case L'Y': UndoRedo(true); return 0;
                    case L'N': if (ShouldOpenNewWindow()) LaunchNewWindow(L""); else NewFile(); return 0;
                    case L'O': OpenDialog(); return 0;
                    case L'S': if (shift) SaveFile(true); else SaveFile(false); return 0;
                    case L'F': OpenFind(false); return 0;
                    case L'H': OpenFind(true); return 0;
                    case L'A': SelectAll(); return 0;
                    case L'C': CopySel(false); return 0;
                    case L'X': CopySel(true); return 0;
                    case L'V': Paste(); return 0;
                    case L'T': ToggleTopmost(); return 0;
                    case L'W': PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
                    default: break;
                }
            }
            switch (wp) {
                case VK_F3:      FindRepeat(!shift); return 0;
                case VK_BACK:    if (ctrl) KeyBackspaceWord(); else KeyBackspace(); return 0;
                case VK_DELETE:  if (ctrl) KeyDeleteWord(); else KeyDelete(); return 0;
                case VK_RETURN:  InsertText(L"\n", false); return 0;
                case VK_TAB:     InsertText(L"    ", false); return 0;
                case VK_LEFT:    MoveHoriz(-1, shift); return 0;
                case VK_RIGHT:   MoveHoriz(1, shift);  return 0;
                case VK_UP:      MoveVert(-1, shift);  return 0;
                case VK_DOWN:    MoveVert(1, shift);   return 0;
                case VK_HOME:    MoveHomeEnd(false, ctrl, shift); return 0;
                case VK_END:     MoveHomeEnd(true, ctrl, shift);  return 0;
                case VK_PRIOR:   PageMove(-1, shift); return 0;
                case VK_NEXT:    PageMove(1, shift);  return 0;
                case VK_ESCAPE:  ClearSel(); AfterMove(true); return 0;
                default: break;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            POINT pt = { x, y };

            if (g.find.open && y >= g.toolbarH && y < TextTop()) {
                FindLayout fl = FindRects();
                if (PtInRect(&fl.field, pt)) {
                    g.find.activeField = 0;
                    SetFocus(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (g.find.replaceMode && PtInRect(&fl.repField, pt)) {
                    g.find.activeField = 1;
                    SetFocus(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (PtInRect(&fl.close, pt)) {
                    CloseFind();
                } else if (PtInRect(&fl.aa, pt)) {
                    g.find.caseSensitive = !g.find.caseSensitive;
                    g.find.dirtyHits = true;
                    g.find.current = -1;
                    if (!g.find.q.empty()) FindNav(true);
                    InvalidateRect(hwnd, nullptr, FALSE);
                } else if (PtInRect(&fl.up, pt)) {
                    FindNav(false);
                } else if (PtInRect(&fl.down, pt)) {
                    FindNav(true);
                } else if (g.find.replaceMode && PtInRect(&fl.repl, pt)) {
                    ReplaceCurrent();
                } else if (g.find.replaceMode && PtInRect(&fl.replAll, pt)) {
                    ReplaceAll();
                } else if (g.find.replaceMode && PtInRect(&fl.fromStart, pt)) {
                    g.find.fromStart = !g.find.fromStart;
                    if (!g.find.q.empty()) { g.find.current = -1; FindNav(true); }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            if (y < g.toolbarH) {
                RECT fn = FileNameRect();
                if (PtInRect(&fn, pt)) { StartRename(); return 0; }
                int id = HitButton(x, y);
                if (id) {
                    g.downBtn = id;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    switch (id) {
                        case 1: OpenDialog(); break;
                        case 2: SaveFile(false); break;
                        case 3: SaveFile(true); break;
                        case 4: ToggleTopmost(); break;
                        case 5: ToggleWrap(); break;
                    }
                    g.downBtn = 0;
                    g.hoverBtn = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }

            if (y >= g.clientH - g.statusH) {
                StatusLayout L = StatusRects(MeasureDC(), g.clientH - g.statusH);
                if (PtInRect(&L.enc, pt)) { ShowEncodingMenu(pt); return 0; }
                if (PtInRect(&L.eol, pt)) { ShowEolMenu(pt); return 0; }
                return 0;
            }

            if (y >= TextTop()) {
                SetFocus(hwnd);
                SetCapture(hwnd);
                g.selecting = true;
                MouseToCaret(x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (WantTextArea(y)) {
                SetFocus(hwnd);
                SetCapture(hwnd);
                g.selecting = true;
                MouseToCaret(x, y, false);
                SelectWordAt(g.curL, g.curC);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g.downBtn) {
                g.downBtn = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (g.selecting) { g.selecting = false; ReleaseCapture(); }
            return 0;
        }

        case WM_RBUTTONUP: {
            int y = GET_Y_LPARAM(lp);
            if (WantTextArea(y)) {
                POINT pt = { GET_X_LPARAM(lp), y };
                SetFocus(hwnd);
                ShowContextMenu(pt);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            // 查找栏悬停
            if (g.find.open && y >= g.toolbarH && y < TextTop()) {
                POINT pt = { x, y };
                FindLayout fl = FindRects();
                int fb = 0;
                if (PtInRect(&fl.close, pt)) fb = 4;
                else if (PtInRect(&fl.aa, pt)) fb = 3;
                else if (PtInRect(&fl.up, pt)) fb = 1;
                else if (PtInRect(&fl.down, pt)) fb = 2;
                else if (g.find.replaceMode && PtInRect(&fl.repl, pt)) fb = 5;
                else if (g.find.replaceMode && PtInRect(&fl.replAll, pt)) fb = 6;
                else if (g.find.replaceMode && PtInRect(&fl.fromStart, pt)) fb = 7;
                if (fb != g.hoverFindBtn) {
                    g.hoverFindBtn = fb;
                    RECT fr = { 0, g.toolbarH, g.clientW, TextTop() };
                    InvalidateRect(hwnd, &fr, FALSE);
                }
            } else if (g.hoverFindBtn) {
                g.hoverFindBtn = 0;
                RECT fr = { 0, g.toolbarH, g.clientW, g.toolbarH + g.findH };
                InvalidateRect(hwnd, &fr, FALSE);
            }
            // 工具栏悬停
            bool overName = false;
            int h = 0;
            if (y < g.toolbarH) {
                RECT fn = FileNameRect();
                POINT pt = { x, y };
                overName = PtInRect(&fn, pt) != 0;
                h = overName ? 0 : HitButton(x, y);
            }
            if (h != g.hoverBtn || overName != g.hoverName) {
                g.hoverBtn = h;
                g.hoverName = overName;
                RECT tr = { 0, 0, g.clientW, g.toolbarH };
                InvalidateRect(hwnd, &tr, FALSE);
            }
            // 拖拽选择 + 边缘自动滚动
            if (g.selecting && (wp & MK_LBUTTON)) {
                int cy = y;
                if (y < TextTop() && g.topL > 0) {
                    --g.topL;
                    UpdateScroll();
                    cy = TextTop() + 1;
                } else if (y > TextBottom() && g.topL < (size_t)MaxTop()) {
                    ++g.topL;
                    UpdateScroll();
                    cy = TextBottom() - 1;
                }
                MouseToCaret(x, std::clamp(cy, TextTop(), std::max(TextTop(), TextBottom() - 1)), true);
            }
            return 0;
        }

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                SetCursor(LoadCursorW(nullptr, WantTextArea(pt.y) ? IDC_IBEAM : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl) { ZoomFont(delta > 0 ? 1 : -1); return 0; }
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == 0) lines = 1;
            int steps = -(delta * (int)lines) / WHEEL_DELTA;
            if (steps == 0) steps = (delta > 0 ? -1 : 1);
            if (shift) {
                g.xOffset = std::max(0, g.xOffset - steps * g.charW * 4);
            } else {
                int nt = std::clamp((int)g.topL + steps, 0, MaxTop());
                g.topL = (size_t)nt;
                UpdateScroll();
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_VSCROLL: {
            SCROLLINFO si = { 0 };
            si.cbSize = sizeof(si);
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS;
            if (!GetScrollInfo(hwnd, SB_VERT, &si)) return 0;
            int pos = si.nPos;
            switch (LOWORD(wp)) {
                case SB_TOP:            pos = si.nMin; break;
                case SB_BOTTOM:         pos = si.nMax; break;
                case SB_LINEUP:         --pos; break;
                case SB_LINEDOWN:       ++pos; break;
                case SB_PAGEUP:         pos -= (int)si.nPage; break;
                case SB_PAGEDOWN:       pos += (int)si.nPage; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION:  pos = si.nTrackPos; break;
                default: return 0;
            }
            g.topL = (size_t)std::clamp(pos, 0, MaxTop());
            UpdateScroll();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_DROPFILES: {
            HDROP hd = (HDROP)wp;
            wchar_t p[32768] = { 0 };
            if (DragQueryFileW(hd, 0, p, 32768)) {
                DragFinish(hd);
                if (ShouldOpenNewWindow()) LaunchNewWindow(p);
                else LoadFile(p);
            } else {
                DragFinish(hd);
            }
            return 0;
        }

        case WM_CLOSE:
            CaptureWindowRect();
            SaveSettings();
            if (ConfirmDiscard()) DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            KillTimer(hwnd, 2);
            KillTimer(hwnd, 3);
            if (g.hMono)   DeleteObject(g.hMono);
            if (g.hUI)     DeleteObject(g.hUI);
            if (g.hUIBold) DeleteObject(g.hUIBold);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
// 入口
// -----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    typedef BOOL (WINAPI* DPICTX)(void*);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        DPICTX fn = (DPICTX)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn) fn((void*)(INT_PTR)-4);
        else SetProcessDPIAware();
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(hrCo);

    g.hinst = hInst;
    LoadSettings();

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = L"BareWindowClass";
    if (!RegisterClassExW(&wc)) {
        if (needUninit) CoUninitialize();
        return 1;
    }

    RECT wr = g.winRect;
    if (wr.right - wr.left < 200 || wr.bottom - wr.top < 150) {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int w = std::min(1000, sw - 80), h = std::min(680, sh - 80);
        wr.left = (sw - w) / 2;
        wr.top = (sh - h) / 2;
        wr.right = wr.left + w;
        wr.bottom = wr.top + h;
    } else {
        ClampToWorkArea(wr);
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"未命名 — Bare",
                                WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        if (needUninit) CoUninitialize();
        return 1;
    }
    if (g.topmost)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ShowWindow(hwnd, g.winMax ? SW_SHOWMAXIMIZED : SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (needUninit) CoUninitialize();
    return 0;
}




