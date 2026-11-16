// =============================================================================
//  Bare — 极简文本编辑器（单文件 Win32 / C++17）
//  单文件 Win32 原生实现：无运行时依赖、内存映射加载、只绘制可视行
//  功能边界：打开 / 编辑 / 保存 / 查找。无标签页、无设置面板、无插件。
//  编译： build-msvc.bat (MSVC) / build.bat (MinGW)
// =============================================================================
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commdlg.h>
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
    enum Kind { UTF8, UTF16LE, UTF16BE, ANSI, BYTES } kind = UTF8;
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
// 查找状态：内嵌查找栏
// -----------------------------------------------------------------------------
struct FindState {
    bool open = false;
    std::wstring q;                               // 查询串
    bool caseSensitive = false;                   // 区分大小写
    std::vector<std::pair<size_t, size_t>> hits;  // (行, 起始列)
    int current = -1;                             // 当前命中下标
    bool dirtyHits = true;                        // 命中表需要重建
    bool truncated = false;                       // 命中过多被截断
};

// -----------------------------------------------------------------------------
// 全局状态（单窗口、单文档）
// -----------------------------------------------------------------------------
struct App {
    HWND hwnd = nullptr;
    HINSTANCE hinst = nullptr;
    HFONT hMono = nullptr, hUI = nullptr, hUIBold = nullptr;

    std::vector<std::wstring> lines;   // 每行保留行尾 '\r'（CRLF）
    std::wstring path;
    Enc enc;
    bool dirty = false;
    bool endsWithNL = false;           // 文件末尾是否有换行
    bool crlfStyle = true;             // 新插入的换行是否使用 CRLF
    unsigned long long fileSize = 0;

    size_t curL = 0, curC = 0;         // 光标：行 / 列（列是原始 wchar 下标）
    bool hasSel = false;
    size_t anchorL = 0, anchorC = 0;
    bool selecting = false;            // 鼠标拖拽中
    int desiredX = -1;                // 上下移动时的目标像素列
    wchar_t pendingHigh = 0;          // 等待低代理项

    size_t topL = 0;
    int xOffset = 0;
    int maxLinePx = 0;

    int dpi = 96, fontPx = 16, lineH = 22, charW = 9;
    int gutterW = 56, toolbarH = 44, statusH = 26;
    int clientW = 0, clientH = 0;

    int hoverBtn = 0, downBtn = 0;     // 1=打开 2=保存 3=另存为
    int hoverFindBtn = 0;              // 查找栏按钮：1=上一个 2=下一个 3=Aa 4=关闭
    FindState find;
    int findH = 40;                    // 查找栏高度（随 DPI 缩放）
    bool hasFocus = true, caretOn = true;
    bool dark = false;

    COLORREF cBg = 0, cText = 0, cGutter = 0, cGutterText = 0;
    COLORREF cBar = 0, cBarText = 0, cSep = 0, cStatusText = 0;
    COLORREF cBtn = 0, cBtnHover = 0, cBtnDown = 0, cBtnBorder = 0;
    COLORREF cSel = 0, cSelText = 0, cCurLine = 0, cCaret = 0;
    COLORREF cField = 0, cFieldBorder = 0, cFieldText = 0, cPh = 0;
    COLORREF cFind = 0, cFindText = 0, cAccent = 0;
};
static App g;

// 前向声明
static void UpdateTitle();
static void UpdateScroll();
static void EnsureCaretVisible();
static void UpdateImePos();
static bool ConfirmDiscard();
static void LoadFile(const std::wstring& p);
static void SaveFile(bool saveAs);
static void OpenDialog();
static void Refresh();
static void AfterEdit();
static void AfterMove(bool resetDesired);

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------
static inline bool IsHigh(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
static inline bool IsLow(wchar_t c)  { return c >= 0xDC00 && c <= 0xDFFF; }

static std::wstring FileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
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

// 可见长度：行尾 '\r' 属于换行符，不参与编辑与显示
static inline size_t VLen(const std::wstring& s) {
    return s.size() - ((!s.empty() && s.back() == L'\r') ? 1 : 0);
}

// 显示串：与原始行等长，控制字符画成中点，保证列下标一一对应
static std::wstring Disp(const std::wstring& s) {
    std::wstring d(s.data(), VLen(s));
    for (wchar_t& c : d)
        if (c < 32 || (c >= 0x7F && c <= 0x9F)) c = 0x00B7; // ·
    return d;
}

// 一次拿到每个字符的累计宽度（GetTextExtentExPointW 比逐字测量快得多）
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

// 像素 x -> 最近的列
static size_t ColFromX(const std::wstring& d, const std::vector<int>& dx, int px) {
    if (d.empty() || px <= 0) return 0;
    auto it = std::upper_bound(dx.begin() + 1, dx.end(), px);
    size_t c = (size_t)(it - dx.begin()) - 1;
    size_t n = d.size();
    if (c >= n) return n;
    if (c + 1 <= n && (px - dx[c]) > (dx[c + 1] - px)) ++c;
    return std::min(c, n);
}

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

// -----------------------------------------------------------------------------
// 编码探测 / 解码：任意文件都能无损打开
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
    // 无 BOM 的 UTF-16：看零字节分布
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
    e.kind = Enc::BYTES;   // 二进制：逐字节映射，保存时按字节还原
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

static void BuildLines(std::wstring& all, const Enc& e, size_t hint) {
    (void)e;
    g.lines.clear();
    if (hint > 1) g.lines.reserve(hint);
    g.endsWithNL = (!all.empty() && all.back() == L'\n');
    size_t start = 0, nlCount = 0, crCount = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] == L'\n') {
            g.lines.emplace_back(all.data() + start, i - start);
            if (i > start && all[i - 1] == L'\r') ++crCount;
            ++nlCount;
            start = i + 1;
        }
    }
    if (start < all.size()) g.lines.emplace_back(all.data() + start, all.size() - start);
    if (g.lines.empty()) g.lines.emplace_back();
    g.crlfStyle = (nlCount == 0) || (crCount * 2 >= nlCount);
    std::wstring().swap(all);
}

// -----------------------------------------------------------------------------
// 写文件（带 64KB 缓冲，逐行编码）
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

// -----------------------------------------------------------------------------
// 文件：打开 / 保存
// -----------------------------------------------------------------------------
static void LoadFile(const std::wstring& p) {
    if (!ConfirmDiscard()) return;

    HANDLE hf = CreateFileW(p.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
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

    Enc e = DetectEncoding(data, n);
    // 先按换行符估算行数，一次性预留行表，避免大文件反复扩容拷贝
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
    BuildLines(all, e, hint);
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
    g.find.open = false;               // 换文件时收起查找栏
    g.find.hits.clear();
    g.find.current = -1;
    g.find.dirtyHits = true;
    g.find.truncated = false;
    UpdateTitle();
    UpdateScroll();
    Refresh();
}

static void OpenDialog() {
    wchar_t file[32768] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0文本文件 (*.txt)\0*.txt\0\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) LoadFile(file);
}

static bool AskSavePath() {
    wchar_t file[32768] = { 0 };
    if (!g.path.empty()) wcsncpy(file, g.path.c_str(), 32767);
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
    g.path = file;
    return true;
}

static void SaveFile(bool saveAs) {
    if (saveAs || g.path.empty()) {
        if (!AskSavePath()) return;
    }
    HANDLE hf = CreateFileW(g.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        MessageBoxW(g.hwnd, L"保存失败：文件可能被占用或路径不可写。",
                    L"Bare", MB_OK | MB_ICONWARNING);
        return;
    }
    Writer w(hf);
    WriteBOM(w, g.enc);
    const std::wstring nl = L"\n";
    for (size_t i = 0; i < g.lines.size() && w.ok; ++i) {
        WriteEncoded(w, g.lines[i], g.enc);
        if (i + 1 < g.lines.size() || g.endsWithNL) WriteEncoded(w, nl, g.enc);
    }
    w.Flush();
    LARGE_INTEGER li = { { 0, 0 } };
    GetFileSizeEx(hf, &li);
    g.fileSize = (unsigned long long)li.QuadPart;
    CloseHandle(hf);

    if (!w.ok) {
        MessageBoxW(g.hwnd, L"写入文件时发生错误。", L"Bare", MB_OK | MB_ICONWARNING);
        return;
    }
    g.dirty = false;
    UpdateTitle();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static bool ConfirmDiscard() {
    if (!g.dirty) return true;
    int r = MessageBoxW(g.hwnd, L"当前内容尚未保存，是否保存？", L"Bare",
                        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) {
        SaveFile(false);
        if (g.dirty) return false;   // 另存为被取消
    }
    return true;
}

// 新建空白文档
static void NewFile() {
    if (!ConfirmDiscard()) return;
    g.path.clear();
    g.enc = Enc();
    g.dirty = false;
    g.fileSize = 0;
    g.endsWithNL = false;
    g.crlfStyle = true;
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
    g.find.open = false;
    g.find.hits.clear();
    g.find.current = -1;
    g.find.dirtyHits = true;
    g.find.truncated = false;
    UpdateTitle();
    UpdateScroll();
    Refresh();
}

// -----------------------------------------------------------------------------
// 编辑核心
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

static bool DeleteSelection() {
    if (!HasSelection()) { g.hasSel = false; return false; }
    size_t l1, c1, l2, c2;
    GetSel(l1, c1, l2, c2);
    std::wstring tail = g.lines[l2].substr(c2);
    if (l1 == l2) {
        g.lines[l1] = g.lines[l1].substr(0, c1) + tail;
    } else {
        g.lines[l1] = g.lines[l1].substr(0, c1) + tail;
        g.lines.erase(g.lines.begin() + l1 + 1, g.lines.begin() + l2 + 1);
    }
    g.curL = l1; g.curC = c1;
    ClearSel();
    return true;
}

static void InsertText(const std::wstring& s) {
    if (s.empty()) return;
    DeleteSelection();

    std::vector<std::wstring> parts;
    size_t st = 0;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == L'\n') { parts.push_back(s.substr(st, i - st)); st = i + 1; }
    parts.push_back(s.substr(st));

    size_t l = g.curL, c = g.curC;
    std::wstring left  = g.lines[l].substr(0, c);
    std::wstring right = g.lines[l].substr(c);   // 含可能的行尾 '\r'
    const std::wstring cr = g.crlfStyle ? L"\r" : L"";

    if (parts.size() == 1) {
        g.lines[l] = left + parts[0] + right;
        g.curC = c + parts[0].size();
    } else {
        g.lines[l] = left + parts[0] + cr;
        size_t pos = l + 1;
        for (size_t i = 1; i + 1 < parts.size(); ++i)
            g.lines.insert(g.lines.begin() + pos++, parts[i] + cr);
        g.lines.insert(g.lines.begin() + pos, parts.back() + right);
        g.curL = pos;
        g.curC = parts.back().size();
    }
}

static void KeyBackspace() {
    if (DeleteSelection()) return;
    if (g.curC > 0) {
        size_t c = g.curC - 1;
        if (c > 0 && IsLow(g.lines[g.curL][c]) && IsHigh(g.lines[g.curL][c - 1])) --c;
        g.lines[g.curL].erase(c, g.curC - c);
        g.curC = c;
    } else if (g.curL > 0) {
        size_t pl = VLen(g.lines[g.curL - 1]);
        g.lines[g.curL - 1] = g.lines[g.curL - 1].substr(0, pl) + g.lines[g.curL];
        g.lines.erase(g.lines.begin() + g.curL);
        --g.curL;
        g.curC = pl;
    } else return;
}

static void KeyDelete() {
    if (DeleteSelection()) return;
    size_t vl = VLen(g.lines[g.curL]);
    if (g.curC < vl) {
        size_t n = 1;
        if (IsHigh(g.lines[g.curL][g.curC]) && g.curC + 1 < vl &&
            IsLow(g.lines[g.curL][g.curC + 1])) n = 2;
        g.lines[g.curL].erase(g.curC, n);
    } else if (g.curL + 1 < g.lines.size()) {
        std::wstring cur = g.lines[g.curL].substr(0, vl);
        g.lines[g.curL] = cur + g.lines[g.curL + 1];
        g.lines.erase(g.lines.begin() + g.curL + 1);
    } else return;
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
            memcpy(p, out.c_str(), (out.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
        CloseClipboard();
    }
    if (cut) { DeleteSelection(); AfterEdit(); }
}

static void Paste() {
    if (!OpenClipboard(g.hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    std::wstring t;
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            std::wstring s(p);
            t.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == L'\r' || s[i] == 0) continue;  // CRLF -> LF
                t += s[i];
            }
        }
        if (p) GlobalUnlock(h);
    }
    CloseClipboard();
    if (!t.empty()) { InsertText(t); AfterEdit(); }
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

static int CaretXIn(HDC hdc) {
    HGDIOBJ of = SelectObject(hdc, g.hMono);
    std::wstring d = Disp(g.lines[g.curL]);
    std::vector<int> dx;
    ComputeDx(hdc, d, dx);
    int x = dx[std::min(g.curC, d.size())];
    SelectObject(hdc, of);
    return x;
}

static void MoveVert(int delta, bool ext) {
    if (ext && !g.hasSel) { g.anchorL = g.curL; g.anchorC = g.curC; }
    HDC hdc = GetDC(g.hwnd);
    HGDIOBJ of = SelectObject(hdc, g.hMono);
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
    SelectObject(hdc, of);
    ReleaseDC(g.hwnd, hdc);
    g.hasSel = ext;
    AfterMove(false);
}

static void MoveHomeEnd(bool end, bool ctrl, bool ext) {
    if (ext && !g.hasSel) { g.anchorL = g.curL; g.anchorC = g.curC; }
    if (ctrl) g.curL = end ? g.lines.size() - 1 : 0;
    g.curC = end ? VLen(g.lines[g.curL]) : 0;
    g.hasSel = ext;
    AfterMove(true);
}

static void PageMove(int dir, bool ext) {
    MoveVert(dir * VisLines(), ext);
}

// -----------------------------------------------------------------------------
// 视图 / 光标 / IME
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
    HDC hdc = GetDC(g.hwnd);
    int cx = CaretXIn(hdc);
    ReleaseDC(g.hwnd, hdc);
    CompositionForm cf = { 0 };
    cf.dwStyle = 0x0002; // CFS_POINT
    cf.ptCurrentPos.x = g.gutterW + cx - g.xOffset;
    cf.ptCurrentPos.y = TextTop() + (int)(g.curL - g.topL) * g.lineH + g.lineH;
    cf.rcArea.left = g.gutterW;
    cf.rcArea.top = TextTop();
    cf.rcArea.right = g.clientW;
    cf.rcArea.bottom = g.clientH - g.statusH;
    setCW(imc, &cf);
    relCtx(imc, g.hwnd);
}

static void EnsureCaretVisible() {
    if (g.lines.empty()) return;
    g.curL = std::min(g.curL, g.lines.size() - 1);
    g.curC = std::min(g.curC, VLen(g.lines[g.curL]));

    int vis = VisLines();
    if ((int)g.curL < (int)g.topL) g.topL = g.curL;
    if (g.curL >= g.topL + (size_t)vis) g.topL = g.curL - vis + 1;
    g.topL = (size_t)std::clamp((int)std::min<size_t>(g.topL, INT_MAX), 0, MaxTop());

    HDC hdc = GetDC(g.hwnd);
    int cx = CaretXIn(hdc);
    ReleaseDC(g.hwnd, hdc);
    int viewW = std::max(40, g.clientW - g.gutterW - 10);
    if (cx - g.xOffset < 0) g.xOffset = cx;
    if (cx - g.xOffset > viewW - 4) g.xOffset = cx - viewW + 40;
    if (g.xOffset < 0) g.xOffset = 0;
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
    g.dirty = true;
    g.desiredX = -1;
    g.pendingHigh = 0;
    if (g.lines.size() == 1 && g.lines[0].empty()) g.endsWithNL = false;
    UpdateTitle();
    Refresh();
}

// -----------------------------------------------------------------------------
// 查找：内嵌查找栏（Ctrl+F / F3）
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
    if (F.current < 0 || (size_t)F.current >= F.hits.size()) {
        size_t n = F.hits.size(), idx = 0;
        if (forward) {
            for (size_t i = 0; i < n; ++i) {
                const auto& h = F.hits[i];
                if (h.first > g.curL || (h.first == g.curL && h.second >= g.curC)) {
                    idx = i; break;
                }
            }
        } else {
            idx = n - 1;
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
    size_t n = F.hits.size();
    size_t idx = forward ? ((size_t)F.current + 1) % n
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

static void OpenFind() {
    if (g.find.open) return;
    g.find.open = true;
    UpdateScroll();
    EnsureCaretVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void CloseFind() {
    if (!g.find.open) return;
    g.find.open = false;
    g.find.hits.clear();
    g.find.current = -1;
    g.find.dirtyHits = true;
    g.find.truncated = false;
    UpdateScroll();
    EnsureCaretVisible();
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void FindRepeat(bool forward) {
    if (g.find.q.empty()) { OpenFind(); return; }
    OpenFind();
    FindNav(forward);
}

static void PasteToFind() {
    if (!OpenClipboard(g.hwnd)) return;
    std::wstring t;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) {
            for (const wchar_t* s = p; *s && t.size() < 200; ++s)
                if (*s >= 32) t.push_back(*s);
        }
        if (p) GlobalUnlock(h);
    }
    CloseClipboard();
    if (t.empty()) return;
    g.find.q += t;
    OnFindChanged();
}

struct FindLayout { RECT field, up, down, aa, close; };

static FindLayout FindRects() {
    const int pad   = MulDiv(14, g.dpi, 96);
    const int bh    = MulDiv(26, g.dpi, 96);
    const int gap   = MulDiv(4, g.dpi, 96);
    const int y     = g.toolbarH + (g.findH - bh) / 2;
    const int right = g.clientW - pad;
    FindLayout L;
    int x = right;
    L.close = { x - bh, y, x, y + bh }; x -= bh + gap * 2;
    int aw = MulDiv(30, g.dpi, 96);
    L.aa = { x - aw, y, x, y + bh }; x -= aw + gap;
    L.down = { x - bh, y, x, y + bh }; x -= bh + gap;
    L.up = { x - bh, y, x, y + bh }; x -= bh + MulDiv(10, g.dpi, 96);
    int fw = MulDiv(300, g.dpi, 96);
    int avail = x - pad;
    if (fw > avail) fw = std::max(0, avail);
    L.field = { pad, y, pad + fw, y + bh };
    return L;
}

// -----------------------------------------------------------------------------
// 界面绘制
// -----------------------------------------------------------------------------
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

static RECT BtnRect(int id, int cw) {
    const int w = MulDiv(72, g.dpi, 96), h = MulDiv(30, g.dpi, 96);
    const int gap = MulDiv(8, g.dpi, 96), right = MulDiv(14, g.dpi, 96);
    int x = cw - right - w;
    if (id == 2) x -= gap + w;
    if (id == 1) x -= 2 * (gap + w);
    int y = (g.toolbarH - h) / 2;
    return RECT{ x, y, x + w, y + h };
}

static void DrawButton(HDC h, int id, const wchar_t* text) {
    RECT r = BtnRect(id, g.clientW);
    int state = (g.downBtn == id) ? 2 : (g.hoverBtn == id ? 1 : 0);
    COLORREF fill = state == 2 ? g.cBtnDown : (state == 1 ? g.cBtnHover : g.cBtn);
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, g.cBtnBorder);
    HGDIOBJ ob = SelectObject(h, br), op = SelectObject(h, pen);
    RoundRect(h, r.left, r.top, r.right, r.bottom,
              MulDiv(8, g.dpi, 96), MulDiv(8, g.dpi, 96));
    SelectObject(h, ob);
    SelectObject(h, op);
    DeleteObject(br);
    DeleteObject(pen);
    SetTextColor(h, g.cBarText);
    HGDIOBJ of = SelectObject(h, g.hUI);
    DrawTextW(h, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(h, of);
}

static void DrawToolbar(HDC h) {
    Fill(h, 0, 0, g.clientW, g.toolbarH, g.cBar);
    Line(h, 0, g.toolbarH - 1, g.clientW, g.toolbarH - 1, g.cSep);

    std::wstring name = g.path.empty() ? L"未命名" : FileName(g.path);
    if (g.dirty) name += L"  •";
    RECT tb1 = BtnRect(1, g.clientW);
    RECT t = { MulDiv(18, g.dpi, 96), 0, tb1.left - MulDiv(16, g.dpi, 96), g.toolbarH };
    SetTextColor(h, g.cBarText);
    HGDIOBJ of = SelectObject(h, g.hUIBold);
    DrawTextW(h, name.c_str(), -1, &t,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(h, of);

    DrawButton(h, 1, L"打开");
    DrawButton(h, 2, L"保存");
    DrawButton(h, 3, L"另存为");
}

static void DrawFindBar(HDC h) {
    const int top = g.toolbarH;
    Fill(h, 0, top, g.clientW, top + g.findH, g.cBar);
    Line(h, 0, top + g.findH - 1, g.clientW, top + g.findH - 1, g.cSep);

    FindLayout L = FindRects();
    HGDIOBJ ofUI = SelectObject(h, g.hUI);
    SetBkMode(h, TRANSPARENT);

    // 输入框
    HBRUSH fb = CreateSolidBrush(g.cField);
    HPEN fp = CreatePen(PS_SOLID, 1, g.cAccent);
    HGDIOBJ ob = SelectObject(h, fb), op = SelectObject(h, fp);
    RoundRect(h, L.field.left, L.field.top, L.field.right, L.field.bottom,
              MulDiv(8, g.dpi, 96), MulDiv(8, g.dpi, 96));
    SelectObject(h, ob);
    SelectObject(h, op);
    DeleteObject(fb);
    DeleteObject(fp);

    RECT tr = { L.field.left + MulDiv(10, g.dpi, 96), L.field.top,
                L.field.right - MulDiv(8, g.dpi, 96), L.field.bottom };
    const bool empty = g.find.q.empty();
    const std::wstring shown = empty ? std::wstring(L"查找…") : g.find.q;
    SetTextColor(h, empty ? g.cPh : g.cFieldText);
    DrawTextW(h, shown.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 输入框内闪烁光标
    if (g.caretOn) {
        SIZE ts = { 0, 0 };
        GetTextExtentPoint32W(h, shown.c_str(), (int)shown.size(), &ts);
        int cx = tr.left + ts.cx + 1;
        if (cx > tr.right - 2) cx = tr.right - 2;
        Fill(h, cx, L.field.top + MulDiv(5, g.dpi, 96),
             cx + std::max(1, MulDiv(2, g.dpi, 96)),
             L.field.bottom - MulDiv(5, g.dpi, 96), g.cAccent);
    }

    // 命中计数
    std::wstring cnt;
    if (!empty) {
        if (g.find.dirtyHits) cnt = L"…";
        else if (g.find.hits.empty()) cnt = L"无结果";
        else {
            int cur = (g.find.current < 0) ? 1 : g.find.current + 1;
            cnt = std::to_wstring(cur) + L"/" + std::to_wstring(g.find.hits.size());
            if (g.find.truncated) cnt += L"+";
        }
    }
    RECT cr = { L.field.right + MulDiv(8, g.dpi, 96), top,
                L.up.left - MulDiv(6, g.dpi, 96), top + g.findH };
    SetTextColor(h, g.cStatusText);
    DrawTextW(h, cnt.c_str(), -1, &cr,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // 右侧按钮
    int id1 = 1;
    auto btn = [&](const RECT& r, int id, const wchar_t* text, bool accent, bool bold) {
        (void)id1;
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

    SelectObject(h, ofUI);
}

static void DrawStatus(HDC h) {
    int y = g.clientH - g.statusH;
    Fill(h, 0, y, g.clientW, g.clientH, g.cBar);
    Line(h, 0, y, g.clientW, y, g.cSep);
    SetTextColor(h, g.cStatusText);
    HGDIOBJ of = SelectObject(h, g.hUI);

    std::wstring left = g.dirty ? L"已修改" : L"就绪";
    left += L"   ·   ";
    left += g.enc.Name();
    RECT lr = { 14, y, 260, g.clientH };
    DrawTextW(h, left.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    std::wstring right = L"Ln " + std::to_wstring(g.curL + 1) +
                         L", Col " + std::to_wstring(g.curC + 1) +
                         L"      " + FmtSize(g.fileSize) +
                         L"      " + std::to_wstring(g.lines.size()) + L" 行";
    RECT rr = { 260, y, g.clientW - 14, g.clientH };
    DrawTextW(h, right.c_str(), -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(h, of);
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
    Line(mem, g.gutterW, textTop, g.gutterW, textBottom, g.cSep);

    HGDIOBJ of = SelectObject(mem, g.hMono);
    int vis = VisLines();
    int x0 = g.gutterW - g.xOffset;
    size_t l1 = 0, c1 = 0, l2 = 0, c2 = 0;
    if (HasSelection()) GetSel(l1, c1, l2, c2);

    SaveDC(mem);
    IntersectClipRect(mem, g.gutterW, textTop, rc.right, textBottom);

    for (int row = 0; row < vis; ++row) {
        size_t i = g.topL + (size_t)row;
        if (i >= g.lines.size()) break;
        int y = textTop + row * g.lineH;

        std::wstring d = Disp(g.lines[i]);
        std::vector<int> dx;
        ComputeDx(mem, d, dx);

        if (i == g.curL) Fill(mem, 0, y, rc.right, y + g.lineH, g.cCurLine);

        if (HasSelection() && i >= l1 && i <= l2) {
            size_t a = (i == l1 ? c1 : 0);
            size_t b = (i == l2 ? c2 : VLen(g.lines[i]));
            a = std::min(a, d.size());
            b = std::min(b, d.size());
            if (b > a) {
                RECT sr = { x0 + dx[a], y, x0 + dx[b], y + g.lineH };
                HBRUSH sb = CreateSolidBrush(g.cSel);
                FillRect(mem, &sr, sb);
                DeleteObject(sb);
            }
        }

        // 查找命中高亮（当前命中由选区颜色标示）
        if (g.find.open && !g.find.q.empty()) {
            const std::wstring& q = g.find.q;
            size_t vl = VLen(g.lines[i]);
            if (vl >= q.size()) {
                int curIdx = g.find.dirtyHits ? -1 : g.find.current;
                for (size_t c = 0; c + q.size() <= vl; ++c) {
                    if (!MatchAt(g.lines[i], c, q)) continue;
                    bool isCur = curIdx >= 0 && (size_t)curIdx < g.find.hits.size() &&
                                 g.find.hits[curIdx].first == i &&
                                 g.find.hits[curIdx].second == c;
                    if (!isCur && c + q.size() < dx.size()) {
                        RECT fr = { x0 + dx[c], y, x0 + dx[c + q.size()], y + g.lineH };
                        HBRUSH hb = CreateSolidBrush(g.cFind);
                        FillRect(mem, &fr, hb);
                        DeleteObject(hb);
                    }
                    c += q.size() - 1;
                }
            }
        }

        if (!d.empty()) {
            SetTextColor(mem, g.cText);
            ExtTextOutW(mem, x0, y + (g.lineH - g.fontPx) / 2, 0, nullptr,
                        d.c_str(), (UINT)d.size(), nullptr);
        }
        if (!dx.empty() && dx.back() > g.maxLinePx) g.maxLinePx = dx.back();
    }

    // 自绘闪烁光标
    if (!g.find.open && g.hasFocus && g.caretOn && g.curL >= g.topL &&
        g.curL < g.topL + (size_t)vis) {
        std::wstring d = Disp(g.lines[g.curL]);
        std::vector<int> dx;
        ComputeDx(mem, d, dx);
        int cx = dx[std::min(g.curC, d.size())];
        int cy = textTop + (int)(g.curL - g.topL) * g.lineH;
        Fill(mem, x0 + cx, cy + 2, x0 + cx + 2, cy + g.lineH - 2, g.cCaret);
    }
    RestoreDC(mem, -1);

    // 行号
    SaveDC(mem);
    IntersectClipRect(mem, 0, textTop, g.gutterW, textBottom);
    SetTextColor(mem, g.cGutterText);
    HGDIOBJ ofNum = SelectObject(mem, g.hUI);
    for (int row = 0; row < vis; ++row) {
        size_t i = g.topL + (size_t)row;
        if (i >= g.lines.size()) break;
        int y = textTop + row * g.lineH;
        wchar_t num[32];
        swprintf(num, 32, L"%llu", (unsigned long long)(i + 1));
        RECT nr = { 0, y, g.gutterW - 10, y + g.lineH };
        DrawTextW(mem, num, -1, &nr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(mem, ofNum);
    RestoreDC(mem, -1);

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
// 输入
// -----------------------------------------------------------------------------
static void MouseToCaret(int x, int y, bool ext) {
    int row = (y - TextTop()) / g.lineH;
    if (row < 0) row = 0;
    size_t l = g.topL + (size_t)row;
    if (l >= g.lines.size()) l = g.lines.size() - 1;

    HDC hdc = GetDC(g.hwnd);
    HGDIOBJ o = SelectObject(hdc, g.hMono);
    std::wstring d = Disp(g.lines[l]);
    std::vector<int> dx;
    ComputeDx(hdc, d, dx);
    size_t c = ColFromX(d, dx, x - g.gutterW + g.xOffset);
    SelectObject(hdc, o);
    ReleaseDC(g.hwnd, hdc);

    if (ext && !g.hasSel) { g.anchorL = g.curL; g.anchorC = g.curC; }
    g.curL = l; g.curC = c;
    g.hasSel = ext;
    g.desiredX = -1;
    Refresh();
}

static int HitButton(int x, int y) {
    for (int id = 1; id <= 3; ++id) {
        RECT r = BtnRect(id, g.clientW);
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return id;
    }
    return 0;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.hwnd = hwnd;
            g.dark = SystemDark();
            InitColors();

            HDC sdc = GetDC(hwnd);
            g.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
            ReleaseDC(hwnd, sdc);
            g.fontPx   = MulDiv(16, g.dpi, 96);
            g.lineH    = g.fontPx + MulDiv(6, g.dpi, 96);
            g.gutterW  = MulDiv(56, g.dpi, 96);
            g.toolbarH = MulDiv(44, g.dpi, 96);
            g.statusH  = MulDiv(26, g.dpi, 96);
            g.findH    = MulDiv(40, g.dpi, 96);

            g.hMono = CreateFontW(-g.fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            g.hUI = CreateFontW(-MulDiv(13, g.dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            g.hUIBold = CreateFontW(-MulDiv(14, g.dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
            HDC hdc = GetDC(hwnd);
            HGDIOBJ o = SelectObject(hdc, g.hMono);
            SIZE sz = { 0, 0 };
            GetTextExtentPoint32W(hdc, L"M", 1, &sz);
            g.charW = sz.cx ? sz.cx : 9;
            SelectObject(hdc, o);
            ReleaseDC(hwnd, hdc);

            BOOL dark = g.dark ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                                  &dark, sizeof(dark));

            g.lines.emplace_back();
            g.endsWithNL = false;
            g.crlfStyle = true;
            DragAcceptFiles(hwnd, TRUE);
            SetTimer(hwnd, 1, 530, nullptr);
            UpdateTitle();
            UpdateScroll();

            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            if (argv) {
                if (argc > 1) LoadFile(argv[1]);
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
            g.hasFocus = true; g.caretOn = true; UpdateImePos();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KILLFOCUS:
            g.hasFocus = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_TIMER:
            if (wp == 1) {
                g.caretOn = !g.caretOn;
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wp == 2) {
                KillTimer(hwnd, 2);
                if (g.find.open && g.find.dirtyHits) FindNav(true);
            }
            return 0;

        case WM_IME_STARTCOMPOSITION:
            UpdateImePos();
            break;

        case WM_CHAR: {
            wchar_t ch = (wchar_t)wp;
            if (ch < 32) return 0;
            if (g.find.open) {                 // 查找栏输入
                g.find.q.push_back(ch);
                OnFindChanged();
                return 0;
            }
            if (IsHigh(ch)) { g.pendingHigh = ch; return 0; }
            std::wstring s;
            if (IsLow(ch) && g.pendingHigh) { s += g.pendingHigh; g.pendingHigh = 0; }
            else g.pendingHigh = 0;
            s += ch;
            InsertText(s);
            AfterEdit();
            return 0;
        }

        case WM_KEYDOWN: {
            bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            bool navKey = (wp == VK_RETURN || wp == VK_BACK || wp == VK_ESCAPE ||
                           wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN ||
                           wp == VK_HOME || wp == VK_END || wp == VK_PRIOR || wp == VK_NEXT);
            if (navKey && ImeComposing()) break;   // 组合期间交给输入法选词

            if (g.find.open) {                     // 查找栏独占键盘
                if (wp == VK_ESCAPE) { CloseFind(); return 0; }
                if (wp == VK_RETURN || wp == VK_F3) { FindNav(!shift); return 0; }
                if (wp == VK_BACK) {
                    if (!g.find.q.empty()) { g.find.q.pop_back(); OnFindChanged(); }
                    return 0;
                }
                if (ctrl) {
                    switch (wp) {
                        case L'V': PasteToFind(); return 0;
                        case L'S': if (shift) SaveFile(true); else SaveFile(false); return 0;
                        case L'O': OpenDialog(); return 0;
                        case L'N': NewFile(); return 0;
                        default: break;
                    }
                }
                return 0;
            }

            if (ctrl) {
                switch (wp) {
                    case L'N': NewFile(); return 0;
                    case L'O': OpenDialog(); return 0;
                    case L'S': if (shift) SaveFile(true); else SaveFile(false); return 0;
                    case L'F': OpenFind(); return 0;
                    case L'A': SelectAll(); return 0;
                    case L'C': CopySel(false); return 0;
                    case L'X': CopySel(true); return 0;
                    case L'V': Paste(); return 0;
                    default: break;
                }
            }
            switch (wp) {
                case VK_F3:      FindRepeat(!shift); return 0;
                case VK_BACK:    KeyBackspace(); AfterEdit(); return 0;
                case VK_DELETE:  KeyDelete();    AfterEdit(); return 0;
                case VK_RETURN:  InsertText(L"\n"); AfterEdit(); return 0;
                case VK_TAB:     InsertText(L"    "); AfterEdit(); return 0;
                case VK_LEFT:    MoveHoriz(-1, shift); return 0;
                case VK_RIGHT:   MoveHoriz(1, shift);  return 0;
                case VK_UP:      MoveVert(-1, shift);  return 0;
                case VK_DOWN:    MoveVert(1, shift);   return 0;
                case VK_HOME:    MoveHomeEnd(false, ctrl, shift); return 0;
                case VK_END:     MoveHomeEnd(true, ctrl, shift);  return 0;
                case VK_PRIOR:   PageMove(-1, shift); return 0;
                case VK_NEXT:    PageMove(1, shift);  return 0;
                case VK_ESCAPE:  g.hasSel = false; AfterMove(true); return 0;
                default: break;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (g.find.open && y < TextTop()) {    // 查找栏点击
                POINT pt = { x, y };
                FindLayout fl = FindRects();
                if (PtInRect(&fl.close, pt)) CloseFind();
                else if (PtInRect(&fl.aa, pt)) {
                    g.find.caseSensitive = !g.find.caseSensitive;
                    g.find.dirtyHits = true;
                    g.find.current = -1;
                    if (!g.find.q.empty()) FindNav(true);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                else if (PtInRect(&fl.up, pt)) FindNav(false);
                else if (PtInRect(&fl.down, pt)) FindNav(true);
                else if (PtInRect(&fl.field, pt)) SetFocus(hwnd);
                return 0;
            }
            int id = HitButton(x, y);
            if (id) {
                g.downBtn = id;
                InvalidateRect(hwnd, nullptr, FALSE);
                if (id == 1) OpenDialog();
                else if (id == 2) SaveFile(false);
                else SaveFile(true);
                g.downBtn = 0;
                g.hoverBtn = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (y >= TextTop() && y < g.clientH - g.statusH) {
                SetFocus(hwnd);
                SetCapture(hwnd);
                g.selecting = true;
                MouseToCaret(x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
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

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            if (g.find.open && y >= g.toolbarH && y < TextTop()) {
                POINT pt = { x, y };
                FindLayout fl = FindRects();
                int fb = PtInRect(&fl.close, pt) ? 4
                       : PtInRect(&fl.aa, pt)    ? 3
                       : PtInRect(&fl.up, pt)    ? 1
                       : PtInRect(&fl.down, pt)  ? 2 : 0;
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
            int h = (y < g.toolbarH) ? HitButton(x, y) : 0;
            if (h != g.hoverBtn) {
                g.hoverBtn = h;
                RECT tr = { 0, 0, g.clientW, g.toolbarH };
                InvalidateRect(hwnd, &tr, FALSE);
            }
            if (g.selecting && (wp & MK_LBUTTON)) MouseToCaret(x, y, true);
            return 0;
        }

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int steps = -delta / WHEEL_DELTA * 3;
            if (steps == 0) steps = (delta > 0 ? -1 : 1);
            if (GetKeyState(VK_SHIFT) & 0x8000) {
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
                LoadFile(p);
            } else DragFinish(hd);
            return 0;
        }

        case WM_CLOSE:
            if (ConfirmDiscard()) DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            if (g.hMono)   DeleteObject(g.hMono);
            if (g.hUI)     DeleteObject(g.hUI);
            if (g.hUIBold) DeleteObject(g.hUIBold);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -----------------------------------------------------------------------------
// 入口
// -----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // 高 DPI 感知（动态调用，兼容旧系统）
    typedef BOOL (WINAPI* DPICTX)(void*);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        DPICTX fn = (DPICTX)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn) fn((void*)(INT_PTR)-4);   // PER_MONITOR_AWARE_V2
        else SetProcessDPIAware();
    }

    g.hinst = hInst;
    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = L"BareWindowClass";
    if (!RegisterClassExW(&wc)) return 1;

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 1000, h = 680;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"未命名 — Bare",
                                WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                (sw - w) / 2, (sh - h) / 2, w, h,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
