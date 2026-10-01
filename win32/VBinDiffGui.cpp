//--------------------------------------------------------------------
//
//   Visual Binary Diff - GUI front end
//
//   A small Win32 GUI for Visual Binary Diff.  It lets you pick the
//   two files to compare, set a few comparison options, and shows the
//   byte-by-byte result in two side-by-side panes (file 1 on the left,
//   file 2 on the right) with the differing bytes highlighted.
//
//   The comparison runs on a worker thread, so the window stays
//   responsive; progress is reported through a progress bar and the
//   status line, and a running comparison can be cancelled.
//
//   This program is free software; you can redistribute it and/or
//   modify it under the terms of the GNU General Public License as
//   published by the Free Software Foundation; either version 2 of
//   the License, or (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program; if not, see <https://www.gnu.org/licenses/>.
//
//--------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

#include <string>
#include <vector>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef EM_GETSCROLLPOS
#define EM_GETSCROLLPOS (WM_USER + 221)
#endif
#ifndef EM_SETSCROLLPOS
#define EM_SETSCROLLPOS (WM_USER + 222)
#endif

// Messages posted by the worker thread
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)

//====================================================================
// Display format (mirrors VBinDiff's console layout), one line per
// 16-byte block, shown in both panes:
//
//   00000000  48 65 6C 6C 6F 20 57 6F 72 6C 64 21 0A 00 AA BB |Hello World!....|
//
// Lines end with a single CR: RichEdit normalizes CRLF into CR, so
// using CR keeps our character offsets in sync with the control.
//====================================================================

static const int kLineWidth  = 16;                          // bytes per line
static const int kPrefixLen  = 10;                          // "XXXXXXXX  "
static const int kHexLen     = kLineWidth * 3;              // 48
static const int kAsciiStart = kPrefixLen + kHexLen + 1;    // 59
static const int kLineLen    = kAsciiStart + kLineWidth + 1;// 76

static const wchar_t kHexDigits[] = L"0123456789ABCDEF";

// Hard cap on the text we hand to one pane.  Without it a huge file
// shown in full ("only differing blocks" off) could exhaust memory.
static const size_t kMaxOutChars = 8u * 1024u * 1024u;      // 8 M chars

// Upper bound on the number of coloured ranges applied to one pane.
// Setting character formatting is by far the slowest part of showing a
// big result, so we stop after this many ranges.
static const size_t kMaxRedRuns = 30000;

// Report progress at most once per this many bytes
static const unsigned long long kReportEvery = 1ull << 20;  // 1 MB

//====================================================================
// Control IDs

enum {
  IDC_FILE1_EDIT = 1001,
  IDC_FILE1_BROWSE,
  IDC_FILE2_EDIT,
  IDC_FILE2_BROWSE,
  IDC_OFFSET_EDIT,
  IDC_END_EDIT,
  IDC_MAXLINES_EDIT,
  IDC_ONLYDIFF_CHK,
  IDC_COMPARE_BTN,
  IDC_SWAP_BTN,
  IDC_CLEAR_BTN,
  IDC_PANE1,
  IDC_PANE2,
  IDC_STATUS_TEXT,
  IDC_LABEL1,
  IDC_LABEL2,
  IDC_PROGRESS,
  IDC_PERCENT
};

// Application icon (defined in VBinDiffGui.rc)
#define IDI_APPICON 101

// Layout metrics
static const int kTopAreaH = 152;   // height of the control area
static const int kLabelH   = 20;    // pane caption height
static const int kStatusH  = 28;    // height reserved for the status bar
static const int kMargin   = 10;
static const int kLabelW   = 68;
static const int kBrowseW  = 78;
static const int kGap      = 8;

//====================================================================
// Globals

static HINSTANCE g_hInst = NULL;
static HWND      g_hWnd  = NULL;

static HWND g_hFile1Edit    = NULL;
static HWND g_hFile1Browse  = NULL;
static HWND g_hFile2Edit    = NULL;
static HWND g_hFile2Browse  = NULL;
static HWND g_hOffsetEdit   = NULL;
static HWND g_hEndEdit      = NULL;
static HWND g_hMaxLinesEdit = NULL;
static HWND g_hOnlyDiffChk  = NULL;
static HWND g_hCompareBtn   = NULL;
static HWND g_hSwapBtn      = NULL;
static HWND g_hClearBtn     = NULL;
static HWND g_hPane1        = NULL;
static HWND g_hPane2        = NULL;
static HWND g_hLabel1       = NULL;
static HWND g_hLabel2       = NULL;
static HWND g_hStatus       = NULL;
static HWND g_hProgress     = NULL;
static HWND g_hPercent      = NULL;

static HFONT g_hFontUi = NULL;

//====================================================================
// Small helpers

static std::wstring GetCtlText(HWND h)
{
  int len = GetWindowTextLengthW(h);
  if (len <= 0) return L"";

  std::wstring s(len + 1, L'\0');
  GetWindowTextW(h, &s[0], len + 1);
  s.resize(wcslen(s.c_str()));
  return s;
} // end GetCtlText

static std::wstring Trim(const std::wstring& s)
{
  const wchar_t* ws = L" \t\r\n";
  size_t a = s.find_first_not_of(ws);
  if (a == std::wstring::npos) return L"";
  size_t b = s.find_last_not_of(ws);
  return s.substr(a, b - a + 1);
} // end Trim

static std::wstring BaseName(const std::wstring& path)
{
  size_t p = path.find_last_of(L"\\/");
  return (p == std::wstring::npos) ? path : path.substr(p + 1);
} // end BaseName

// Parse a decimal or 0x-prefixed hex number.  Returns false on garbage.
static bool ParseNumber(const std::wstring& text, unsigned long long& value)
{
  std::wstring t = Trim(text);
  if (t.empty()) return false;

  int base = 10;
  size_t i = 0;

  if (t.size() > 2 && t[0] == L'0' && (t[1] == L'x' || t[1] == L'X')) {
    base = 16;
    i = 2;
  }

  wchar_t* end = NULL;
  value = _wcstoui64(t.c_str() + i, &end, base);
  if (end == t.c_str() + i) return false;   // no digits consumed

  while (*end && iswspace(*end)) ++end;
  return (*end == L'\0');
} // end ParseNumber

static std::wstring FormatNum(unsigned long long v)
{
  wchar_t raw[32];
  swprintf(raw, 32, L"%llu", v);

  std::wstring s(raw);
  std::wstring out;
  int count = 0;

  for (int i = (int)s.size() - 1; i >= 0; --i) {
    out.insert(out.begin(), s[i]);
    if (++count % 3 == 0 && i > 0) out.insert(out.begin(), L',');
  }

  return out;
} // end FormatNum

static bool GetFileSize64(const std::wstring& path, unsigned long long& size)
{
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return false;

  LARGE_INTEGER li;
  BOOL ok = GetFileSizeEx(h, &li);
  CloseHandle(h);
  if (!ok) return false;

  size = (unsigned long long)li.QuadPart;
  return true;
} // end GetFileSize64

//--------------------------------------------------------------------
// Buffered reader (avoids one ReadFile per 16 bytes)

class Reader
{
 public:
  Reader() : file(INVALID_HANDLE_VALUE), pos(0), len(0), atEnd(true) {}

  ~Reader() { Close(); }

  bool Open(const std::wstring& path, unsigned long long start)
  {
    Close();

    file = CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)start;
    if (!SetFilePointerEx(file, li, NULL, FILE_BEGIN)) { Close(); return false; }

    buf.resize(262144);                 // 256 KB
    pos = len = 0;
    atEnd = false;
    return true;
  }

  void Close()
  {
    if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
    pos = len = 0;
    atEnd = true;
  }

  // Returns the number of bytes actually read (may be < want at EOF).
  int Read(unsigned char* out, int want)
  {
    int got = 0;

    while (got < want) {
      if (pos >= len) {
        if (atEnd) break;

        DWORD bytesRead = 0;
        if (!ReadFile(file, &buf[0], (DWORD)buf.size(), &bytesRead, NULL)) { atEnd = true; break; }
        if (bytesRead == 0) { atEnd = true; break; }

        pos = 0;
        len = bytesRead;
      }

      int avail = (int)(len - pos);
      int take  = (want - got < avail) ? (want - got) : avail;

      memcpy(out + got, &buf[pos], take);
      pos += take;
      got += take;
    }

    return got;
  }

 private:
  HANDLE                     file;
  std::vector<unsigned char> buf;
  size_t                     pos, len;
  bool                       atEnd;
}; // end Reader

//====================================================================
// Result rendering

// Append one 16-byte line (hex + ASCII) and remember which characters
// must be drawn in the "difference" colour.
static void AppendDataLine(std::wstring& out,
                           std::vector<std::pair<LONG,LONG> >& redRuns,
                           unsigned long long pos,
                           const unsigned char* data, int count,
                           const unsigned char* other, int otherCount,
                           bool compare)
{
  size_t base = out.size();

  wchar_t prefix[32];
  swprintf(prefix, 32, L"%08llX  ", pos);
  out.append(prefix, kPrefixLen);

  // Hex area
  for (int i = 0; i < kLineWidth; ++i) {
    wchar_t h[3] = { L' ', L' ', L' ' };
    if (i < count) {
      h[0] = kHexDigits[(data[i] >> 4) & 0x0F];
      h[1] = kHexDigits[data[i] & 0x0F];
    }
    out.append(h, 3);
  }

  out.push_back(L'|');

  // ASCII area
  for (int i = 0; i < kLineWidth; ++i) {
    wchar_t c = L' ';
    if (i < count) {
      unsigned char b = data[i];
      c = (b >= 0x20 && b <= 0x7E) ? (wchar_t)b : L'.';
    }
    out.push_back(c);
  }

  out.push_back(L'|');
  out.push_back(L'\r');

  if (!compare) return;

  // Mark the differing bytes.  A byte that is missing on either side
  // counts as different (that is the "one file is longer" case).
  for (int i = 0; i < kLineWidth; ++i) {
    if (i >= count && i >= otherCount) continue;      // neither side has it

    bool differs;
    if (i >= count || i >= otherCount) differs = true;
    else                               differs = (data[i] != other[i]);

    if (!differs) continue;

    if (i < count) {
      // The trailing space is included on purpose: it makes runs of
      // differing bytes merge into one range instead of one range per
      // byte (the space itself has no glyph, so nothing changes
      // visually).  This cuts EM_SETCHARFORMAT calls by ~16x.
      LONG hStart = (LONG)(base + kPrefixLen + i * 3);
      redRuns.push_back(std::make_pair(hStart, hStart + 3));

      LONG aStart = (LONG)(base + kAsciiStart + i);
      redRuns.push_back(std::make_pair(aStart, aStart + 1));
    }
  }
} // end AppendDataLine

// Sort + coalesce touching ranges.  This matters a lot for big results:
// it turns "16 separate red bytes" into one EM_SETCHARFORMAT call.
static void MergeRuns(std::vector<std::pair<LONG,LONG> >& runs)
{
  if (runs.size() < 2) return;

  std::sort(runs.begin(), runs.end());

  size_t w = 0;
  for (size_t i = 0; i < runs.size(); ++i) {
    if (w > 0 && runs[i].first <= runs[w - 1].second) {
      if (runs[i].second > runs[w - 1].second) runs[w - 1].second = runs[i].second;
    } else {
      runs[w++] = runs[i];
    }
  }
  runs.resize(w);
} // end MergeRuns

//--------------------------------------------------------------------
// The result is handed to the RichEdit control as one RTF stream.
// Setting the character formatting range by range used to be the real
// bottleneck for big results (tens of thousands of EM_SETCHARFORMAT
// calls); streaming RTF does the whole document in a single call.

static void RtfAppendEscaped(std::string& rtf, const wchar_t* text, size_t len)
{
  char buf[24];

  for (size_t i = 0; i < len; ++i) {
    wchar_t c = text[i];

    switch (c) {
      case L'\\': rtf += "\\\\";   break;
      case L'{':  rtf += "\\{";    break;
      case L'}':  rtf += "\\}";    break;
      case L'\r': rtf += "\\par "; break;
      case L'\n': break;                      // CR already emitted \par
      default:
        if (c >= 0x20 && c < 0x7F) {
          rtf += (char)c;
        } else if (c >= 0x7F) {
          sprintf(buf, "\\u%d?", (int)(short)c);   // signed 16-bit value
          rtf += buf;
        }
        // any other control character is dropped
        break;
    }
  }
} // end RtfAppendEscaped

struct RtfStream
{
  const char* data;
  size_t      len;
  size_t      pos;
}; // end RtfStream

static DWORD CALLBACK RtfStreamCallback(DWORD_PTR cookie, LPBYTE pbBuff,
                                        LONG cb, LONG* pcb)
{
  RtfStream* s = (RtfStream*)cookie;

  size_t remain = s->len - s->pos;
  size_t take   = ((size_t)cb < remain) ? (size_t)cb : remain;

  if (take) memcpy(pbBuff, s->data + s->pos, take);
  s->pos += take;
  *pcb = (LONG)take;

  return 0;
} // end RtfStreamCallback

static void SetPaneText(HWND hRich, const std::wstring& text,
                        std::vector<std::pair<LONG,LONG> >& redRuns)
{
  SendMessageW(hRich, WM_SETREDRAW, FALSE, 0);
  SetWindowTextW(hRich, L"");

  if (!text.empty()) {
    MergeRuns(redRuns);

    // Never apply an absurd number of ranges.
    if (redRuns.size() > kMaxRedRuns) redRuns.resize(kMaxRedRuns);

    std::string rtf;
    rtf.reserve(text.size() * 2 + 256);
    rtf += "{\\rtf1\\ansi\\deff0"
           "{\\fonttbl{\\f0\\fmodern Consolas;}}"
           "{\\colortbl;\\red0\\green0\\blue0;\\red200\\green0\\blue0;}"
           "\\f0\\fs19\\cf1 ";

    size_t cur = 0;
    for (size_t i = 0; i < redRuns.size(); ++i) {
      size_t a = (size_t)redRuns[i].first;
      size_t b = (size_t)redRuns[i].second;

      if (a < cur) a = cur;
      if (b <= a)  continue;

      if (a > cur)
        RtfAppendEscaped(rtf, text.c_str() + cur, a - cur);

      rtf += "{\\cf2\\b ";
      RtfAppendEscaped(rtf, text.c_str() + a, b - a);
      rtf += "}";

      cur = b;
    }

    if (cur < text.size())
      RtfAppendEscaped(rtf, text.c_str() + cur, text.size() - cur);

    rtf += "}";

    RtfStream stream;
    stream.data = rtf.c_str();
    stream.len  = rtf.size();
    stream.pos  = 0;

    EDITSTREAM es;
    ZeroMemory(&es, sizeof(es));
    es.dwCookie    = (DWORD_PTR)&stream;
    es.pfnCallback = RtfStreamCallback;

    SendMessageW(hRich, EM_STREAMIN, SF_RTF, (LPARAM)&es);
  }

  CHARRANGE home = { 0, 0 };
  SendMessageW(hRich, EM_EXSETSEL, 0, (LPARAM)&home);
  SendMessageW(hRich, EM_SCROLLCARET, 0, 0);

  SendMessageW(hRich, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(hRich, NULL, TRUE);
} // end SetPaneText

//--------------------------------------------------------------------
// Keep the two panes scrolled together so the lines stay aligned.

static void SyncScroll(HWND src)
{
  HWND dst = (src == g_hPane1) ? g_hPane2 : g_hPane1;
  if (!dst) return;

  POINT ps = { 0, 0 }, pd = { 0, 0 };
  SendMessageW(src, EM_GETSCROLLPOS, 0, (LPARAM)&ps);
  SendMessageW(dst, EM_GETSCROLLPOS, 0, (LPARAM)&pd);

  if (ps.x == pd.x && ps.y == pd.y) return;    // already in sync (stops loops)

  SendMessageW(dst, EM_SETSCROLLPOS, 0, (LPARAM)&ps);
} // end SyncScroll

//====================================================================
// The comparison job runs on a worker thread so the UI never blocks.

struct CompareJob
{
  // --- input
  std::wstring       file1, file2;
  unsigned long long offset;
  unsigned long long end;          // 0 = to end of file
  bool               onlyDiff;
  unsigned long long maxLines;

  // --- output
  std::wstring out1, out2;
  std::vector<std::pair<LONG,LONG> > red1, red2;

  unsigned long long size1, size2;
  unsigned long long totalBlocks, diffBlocks, shownBlocks;
  bool               truncated;
  bool               cancelled;
  DWORD              elapsed;
  std::wstring       error;
}; // end CompareJob

static CompareJob*   g_job    = NULL;
static HANDLE        g_thread = NULL;
static volatile LONG g_cancel = 0;

static void ReportProgress(unsigned long long done, unsigned long long total)
{
  int percent = 0;
  if (total > 0) {
    if (done >= total) percent = 100;
    else               percent = (int)((done * 100) / total);
  }

  PostMessageW(g_hWnd, WM_APP_PROGRESS, (WPARAM)percent,
               (LPARAM)(DWORD)(done & 0xFFFFFFFFu));
} // end ReportProgress

static DWORD WINAPI CompareThread(LPVOID)
{
  CompareJob* job = g_job;

  DWORD startTick = GetTickCount();

  bool hasFile1 = !job->file1.empty();
  bool hasFile2 = !job->file2.empty();

  Reader r1, r2;
  if (hasFile1 && !r1.Open(job->file1, job->offset)) {
    job->error = L"无法读取文件 1。";
    PostMessageW(g_hWnd, WM_APP_DONE, 0, 0);
    return 0;
  }
  if (hasFile2 && !r2.Open(job->file2, job->offset)) {
    job->error = L"无法读取文件 2。";
    PostMessageW(g_hWnd, WM_APP_DONE, 0, 0);
    return 0;
  }

  // Total number of bytes we are going to walk, for the progress bar.
  unsigned long long totalBytes;
  {
    unsigned long long limit;
    if (job->end != 0) {
      limit = job->end;                       // inclusive
    } else {
      limit = (job->size1 > job->size2) ? job->size1 : job->size2;
      if (limit > 0) --limit;                 // inclusive
    }
    totalBytes = (limit >= job->offset) ? (limit - job->offset + 1) : 0;
  }

  job->out1.reserve(1 << 16);
  job->out2.reserve(1 << 16);

  unsigned char b1[kLineWidth], b2[kLineWidth];

  unsigned long long pos        = job->offset;
  unsigned long long lastReport = pos;

  ReportProgress(0, totalBytes);

  // The range is [offset, end] with both ends inclusive, so
  // "0 to 0xFFF" covers 4096 bytes (0x000 .. 0xFFF).
  while (true) {
    if (InterlockedCompareExchange(&g_cancel, 0, 0) != 0) { job->cancelled = true; break; }

    if (job->end != 0 && pos > job->end) break;

    int want = kLineWidth;
    if (job->end != 0 && pos + (unsigned long long)want > job->end + 1)
      want = (int)(job->end + 1 - pos);

    int n1 = hasFile1 ? r1.Read(b1, want) : 0;
    int n2 = hasFile2 ? r2.Read(b2, want) : 0;

    if (n1 == 0 && n2 == 0) break;      // both files are exhausted

    ++job->totalBlocks;

    bool differs = false;
    if (hasFile2) differs = ((n1 != n2) || (memcmp(b1, b2, n1) != 0));

    if (differs) ++job->diffBlocks;

    if (!job->onlyDiff || differs) {
      if (job->shownBlocks >= job->maxLines ||
          job->out1.size() > kMaxOutChars) {
        job->truncated = true;
        break;
      }

      AppendDataLine(job->out1, job->red1, pos, b1, n1, b2, n2, hasFile2);

      if (hasFile2)
        AppendDataLine(job->out2, job->red2, pos, b2, n2, b1, n1, true);

      ++job->shownBlocks;
    }

    pos += (unsigned long long)want;

    if (pos - lastReport >= kReportEvery) {
      lastReport = pos;
      ReportProgress(pos - job->offset, totalBytes);
    }
  }

  job->elapsed = GetTickCount() - startTick;

  ReportProgress(totalBytes, totalBytes);

  PostMessageW(g_hWnd, WM_APP_DONE, 0, 0);
  return 0;
} // end CompareThread

//====================================================================
// UI state

static void SetBusy(bool busy)
{
  EnableWindow(g_hFile1Edit,    !busy);
  EnableWindow(g_hFile1Browse,  !busy);
  EnableWindow(g_hFile2Edit,    !busy);
  EnableWindow(g_hFile2Browse,  !busy);
  EnableWindow(g_hOffsetEdit,   !busy);
  EnableWindow(g_hEndEdit,      !busy);
  EnableWindow(g_hMaxLinesEdit, !busy);
  EnableWindow(g_hOnlyDiffChk,  !busy);
  EnableWindow(g_hSwapBtn,      !busy);
  EnableWindow(g_hClearBtn,     !busy);

  SetWindowTextW(g_hCompareBtn, busy ? L"取　消" : L"比　较");

  if (busy) {
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_hPercent, L"0%");
  }
} // end SetBusy

static void FinishCompare()
{
  if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }

  CompareJob* job = g_job;
  g_job = NULL;

  SetBusy(false);

  if (!job) return;

  if (!job->error.empty()) {
    MessageBoxW(g_hWnd, job->error.c_str(), L"VBinDiff GUI", MB_ICONERROR);
    delete job;
    return;
  }

  if (job->cancelled) {
    SetWindowTextW(g_hStatus, L"已取消。");
    SetWindowTextW(g_hPercent, L"");
    delete job;
    return;
  }

  bool hasFile1 = !job->file1.empty();
  bool hasFile2 = !job->file2.empty();
  bool hasFile2Size = hasFile2;

  SendMessageW(g_hProgress, PBM_SETPOS, 100, 0);
  SetWindowTextW(g_hPercent, L"100%");

  bool nothing = job->out1.empty() && job->out2.empty();
  if (nothing) {
    if (job->onlyDiff && hasFile2)
      job->out1 = L"两个文件在指定范围内完全相同，没有差异。\r";
    else
      job->out1 = L"(没有可显示的内容)\r";
  }

  SetWindowTextW(g_hStatus, L"正在生成结果，请稍候...");
  UpdateWindow(g_hStatus);

  SetPaneText(g_hPane1, job->out1, job->red1);
  SetPaneText(g_hPane2, job->out2, job->red2);

  // Pane captions show which file is which.
  SetWindowTextW(g_hLabel1, hasFile1 ? BaseName(job->file1).c_str() : L"文件 1");
  SetWindowTextW(g_hLabel2, hasFile2Size ? BaseName(job->file2).c_str() : L"文件 2");

  // ---- status line ----------------------------------------------
  std::wstring status;

  {
    wchar_t rng[96];
    if (job->end != 0)
      swprintf(rng, 96, L"范围: 0x%llX - 0x%llX (含)    ", job->offset, job->end);
    else
      swprintf(rng, 96, L"范围: 0x%llX - 文件末尾    ", job->offset);
    status += rng;
  }

  if (hasFile1) status += L"文件1: " + FormatNum(job->size1) + L" 字节";
  if (hasFile2) status += L"    文件2: " + FormatNum(job->size2) + L" 字节";
  if (hasFile2) status += L"    差异块: " + FormatNum(job->diffBlocks)
                       + L" / " + FormatNum(job->totalBlocks);
  status += L"    显示: " + FormatNum(job->shownBlocks) + L" 块";
  status += L"    用时: " + FormatNum(job->elapsed) + L" ms";
  if (job->truncated) status += L"    [已达显示上限，结果被截断]";

  SetWindowTextW(g_hStatus, status.c_str());

  delete job;
} // end FinishCompare

//====================================================================
// Comparison (argument checking + job setup)

static void OnCompare(HWND hwnd)
{
  if (g_thread) return;                    // one at a time

  std::wstring file1 = Trim(GetCtlText(g_hFile1Edit));
  std::wstring file2 = Trim(GetCtlText(g_hFile2Edit));
  bool onlyDiff      = (SendMessageW(g_hOnlyDiffChk, BM_GETCHECK, 0, 0) == BST_CHECKED);

  if (file1.empty() && file2.empty()) {
    MessageBoxW(hwnd, L"请至少选择一个要比较的文件。", L"VBinDiff GUI", MB_ICONINFORMATION);
    return;
  }

  // ---- options ---------------------------------------------------
  unsigned long long offset = 0;
  std::wstring offsetText = Trim(GetCtlText(g_hOffsetEdit));
  if (!offsetText.empty() && !ParseNumber(offsetText, offset)) {
    MessageBoxW(hwnd, L"“起始偏移”不是合法的数字（可用十进制或 0x 十六进制）。",
                L"VBinDiff GUI", MB_ICONWARNING);
    return;
  }

  unsigned long long end = 0;
  std::wstring endText = Trim(GetCtlText(g_hEndEdit));
  if (!endText.empty() && !ParseNumber(endText, end)) {
    MessageBoxW(hwnd, L"“结束偏移”不是合法的数字（可用十进制或 0x 十六进制）。",
                L"VBinDiff GUI", MB_ICONWARNING);
    return;
  }

  if (end != 0 && end < offset) {
    MessageBoxW(hwnd, L"“结束偏移”不能小于“起始偏移”。", L"VBinDiff GUI", MB_ICONWARNING);
    return;
  }

  unsigned long long maxLines = 2000;
  std::wstring maxText = Trim(GetCtlText(g_hMaxLinesEdit));
  if (!maxText.empty()) {
    if (!ParseNumber(maxText, maxLines) || maxLines == 0) {
      MessageBoxW(hwnd, L"“最多显示”必须是大于 0 的数字。", L"VBinDiff GUI", MB_ICONWARNING);
      return;
    }
  }

  // ---- sanity check on the files ---------------------------------
  unsigned long long size1 = 0, size2 = 0;
  bool hasFile1 = !file1.empty();
  bool hasFile2 = !file2.empty();

  if (hasFile1 && !GetFileSize64(file1, size1)) {
    std::wstring msg = L"无法打开文件 1:\n" + file1;
    MessageBoxW(hwnd, msg.c_str(), L"VBinDiff GUI", MB_ICONERROR);
    return;
  }
  if (hasFile2 && !GetFileSize64(file2, size2)) {
    std::wstring msg = L"无法打开文件 2:\n" + file2;
    MessageBoxW(hwnd, msg.c_str(), L"VBinDiff GUI", MB_ICONERROR);
    return;
  }

  CompareJob* job = new CompareJob();
  job->file1     = file1;
  job->file2     = file2;
  job->offset    = offset;
  job->end       = end;
  job->onlyDiff  = onlyDiff;
  job->maxLines  = maxLines;
  job->size1     = size1;
  job->size2     = size2;
  job->totalBlocks = job->diffBlocks = job->shownBlocks = 0;
  job->truncated = false;
  job->cancelled = false;
  job->elapsed   = 0;

  g_job = job;
  InterlockedExchange(&g_cancel, 0);

  SetBusy(true);
  SetWindowTextW(g_hStatus, L"正在比较...");
  SetWindowTextW(g_hPane1, L"");
  SetWindowTextW(g_hPane2, L"");
  SetWindowTextW(g_hLabel1, hasFile1 ? BaseName(file1).c_str() : L"文件 1");
  SetWindowTextW(g_hLabel2, hasFile2 ? BaseName(file2).c_str() : L"文件 2");

  g_thread = CreateThread(NULL, 0, CompareThread, NULL, 0, NULL);
  if (!g_thread) {
    g_job = NULL;
    delete job;
    SetBusy(false);
    MessageBoxW(hwnd, L"无法创建比较线程。", L"VBinDiff GUI", MB_ICONERROR);
  }
} // end OnCompare

//====================================================================
// Window plumbing

static void BrowseForFile(HWND hwnd, HWND hEdit)
{
  wchar_t buf[4096];
  buf[0] = L'\0';
  GetWindowTextW(hEdit, buf, 4096);

  OPENFILENAMEW ofn;
  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner   = hwnd;
  ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0\0";
  ofn.lpstrFile   = buf;
  ofn.nMaxFile    = 4096;
  ofn.lpstrTitle  = L"选择要比较的文件";
  ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

  if (GetOpenFileNameW(&ofn))
    SetWindowTextW(hEdit, buf);
} // end BrowseForFile

static HWND CreateCtl(HWND parent, const wchar_t* cls, const wchar_t* text,
                      DWORD style, int id, int x, int y, int w, int h)
{
  HWND ctl = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, NULL);
  if (ctl && g_hFontUi) SendMessageW(ctl, WM_SETFONT, (WPARAM)g_hFontUi, TRUE);
  return ctl;
} // end CreateCtl

static void CreateControls(HWND hwnd)
{
  g_hFontUi = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  const int editX = kMargin + kLabelW;

  // ---- Row 1: file 1
  int y = kMargin;
  CreateCtl(hwnd, L"STATIC", L"文件 1:", SS_LEFT, 0,
            kMargin, y + 4, kLabelW, 20);
  g_hFile1Edit = CreateCtl(hwnd, L"EDIT", L"",
                           WS_BORDER | ES_AUTOHSCROLL, IDC_FILE1_EDIT,
                           editX, y, 600, 25);
  g_hFile1Browse = CreateCtl(hwnd, L"BUTTON", L"浏览...", BS_PUSHBUTTON, IDC_FILE1_BROWSE,
                             editX + 608, y, kBrowseW, 25);

  // ---- Row 2: file 2
  y += 34;
  CreateCtl(hwnd, L"STATIC", L"文件 2:", SS_LEFT, 0,
            kMargin, y + 4, kLabelW, 20);
  g_hFile2Edit = CreateCtl(hwnd, L"EDIT", L"",
                           WS_BORDER | ES_AUTOHSCROLL, IDC_FILE2_EDIT,
                           editX, y, 600, 25);
  g_hFile2Browse = CreateCtl(hwnd, L"BUTTON", L"浏览...", BS_PUSHBUTTON, IDC_FILE2_BROWSE,
                             editX + 608, y, kBrowseW, 25);

  // ---- Row 3: options
  y += 40;
  CreateCtl(hwnd, L"STATIC", L"起始偏移:", SS_LEFT, 0,
            kMargin, y + 4, kLabelW, 20);
  g_hOffsetEdit = CreateCtl(hwnd, L"EDIT", L"0x0",
                            WS_BORDER | ES_AUTOHSCROLL, IDC_OFFSET_EDIT,
                            editX, y, 110, 25);

  CreateCtl(hwnd, L"STATIC", L"结束偏移:", SS_LEFT, 0,
            editX + 122, y + 4, 68, 20);
  g_hEndEdit = CreateCtl(hwnd, L"EDIT", L"0xFFF",
                         WS_BORDER | ES_AUTOHSCROLL, IDC_END_EDIT,
                         editX + 190, y, 110, 25);

  CreateCtl(hwnd, L"STATIC", L"最多显示:", SS_LEFT, 0,
            editX + 312, y + 4, 68, 20);
  g_hMaxLinesEdit = CreateCtl(hwnd, L"EDIT", L"2000",
                              WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, IDC_MAXLINES_EDIT,
                              editX + 380, y, 70, 25);

  g_hOnlyDiffChk = CreateCtl(hwnd, L"BUTTON", L"仅显示有差异的块",
                             BS_AUTOCHECKBOX, IDC_ONLYDIFF_CHK,
                             editX + 462, y + 3, 160, 22);
  SendMessageW(g_hOnlyDiffChk, BM_SETCHECK, BST_CHECKED, 0);

  // ---- Row 4: buttons + progress
  y += 36;
  g_hCompareBtn = CreateCtl(hwnd, L"BUTTON", L"比　较", BS_DEFPUSHBUTTON, IDC_COMPARE_BTN,
                            editX, y, 100, 28);
  g_hSwapBtn = CreateCtl(hwnd, L"BUTTON", L"交换文件", BS_PUSHBUTTON, IDC_SWAP_BTN,
                         editX + 108, y, 100, 28);
  g_hClearBtn = CreateCtl(hwnd, L"BUTTON", L"清空结果", BS_PUSHBUTTON, IDC_CLEAR_BTN,
                          editX + 216, y, 100, 28);

  g_hProgress = CreateCtl(hwnd, PROGRESS_CLASS, L"",
                          PBS_SMOOTH, IDC_PROGRESS,
                          editX + 330, y + 3, 320, 22);
  SendMessageW(g_hProgress, PBM_SETRANGE32, 0, 100);

  g_hPercent = CreateCtl(hwnd, L"STATIC", L"", SS_LEFT, IDC_PERCENT,
                         editX + 658, y + 6, 80, 20);

  // ---- Two result panes
  DWORD paneStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                    ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                    ES_AUTOHSCROLL | ES_NOHIDESEL;

  g_hLabel1 = CreateCtl(hwnd, L"STATIC", L"文件 1", SS_LEFT | SS_ENDELLIPSIS,
                        IDC_LABEL1, kMargin, 0, 100, kLabelH);
  g_hLabel2 = CreateCtl(hwnd, L"STATIC", L"文件 2", SS_LEFT | SS_ENDELLIPSIS,
                        IDC_LABEL2, kMargin, 0, 100, kLabelH);

  g_hPane1 = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", paneStyle,
                             100, 100, 100, 100,
                             hwnd, (HMENU)(INT_PTR)IDC_PANE1, g_hInst, NULL);
  g_hPane2 = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"", paneStyle,
                             100, 100, 100, 100,
                             hwnd, (HMENU)(INT_PTR)IDC_PANE2, g_hInst, NULL);

  if (g_hPane1) {
    SendMessageW(g_hPane1, EM_EXLIMITTEXT, 0, (LPARAM)0x7FFFFFF0);
    SendMessageW(g_hPane1, WM_SETFONT, (WPARAM)g_hFontUi, TRUE);
  }
  if (g_hPane2) {
    SendMessageW(g_hPane2, EM_EXLIMITTEXT, 0, (LPARAM)0x7FFFFFF0);
    SendMessageW(g_hPane2, WM_SETFONT, (WPARAM)g_hFontUi, TRUE);
  }

  // ---- Status
  g_hStatus = CreateCtl(hwnd, L"STATIC", L"请选择两个文件，然后点击“比较”。",
                        SS_LEFT | SS_ENDELLIPSIS, IDC_STATUS_TEXT,
                        kMargin, 0, 100, 20);
} // end CreateControls

static void LayoutControls(HWND hwnd)
{
  RECT rc;
  GetClientRect(hwnd, &rc);

  int clientW = rc.right - rc.left;
  int clientH = rc.bottom - rc.top;

  // The file edits stretch with the window, and the "browse" buttons
  // follow them so they never get covered or pushed off-screen.
  int editX = kMargin + kLabelW;
  int editW = clientW - editX - kGap - kBrowseW - kMargin;
  if (editW < 120) editW = 120;
  int browseX = editX + editW + kGap;

  if (g_hFile1Edit)
    SetWindowPos(g_hFile1Edit, NULL, editX, kMargin, editW, 25, SWP_NOZORDER);
  if (g_hFile1Browse)
    SetWindowPos(g_hFile1Browse, NULL, browseX, kMargin, kBrowseW, 25, SWP_NOZORDER);

  if (g_hFile2Edit)
    SetWindowPos(g_hFile2Edit, NULL, editX, kMargin + 34, editW, 25, SWP_NOZORDER);
  if (g_hFile2Browse)
    SetWindowPos(g_hFile2Browse, NULL, browseX, kMargin + 34, kBrowseW, 25, SWP_NOZORDER);

  // Two side-by-side panes, each half of the client area.
  int paneY = kTopAreaH + kLabelH;
  int paneH = clientH - paneY - kStatusH;
  if (paneH < 60) paneH = 60;

  int paneW = (clientW - kMargin * 3) / 2;
  if (paneW < 100) paneW = 100;

  if (g_hLabel1)
    SetWindowPos(g_hLabel1, NULL, kMargin, kTopAreaH, paneW, kLabelH, SWP_NOZORDER);
  if (g_hLabel2)
    SetWindowPos(g_hLabel2, NULL, kMargin * 2 + paneW, kTopAreaH, paneW, kLabelH, SWP_NOZORDER);

  if (g_hPane1)
    SetWindowPos(g_hPane1, NULL, kMargin, paneY, paneW, paneH, SWP_NOZORDER);
  if (g_hPane2)
    SetWindowPos(g_hPane2, NULL, kMargin * 2 + paneW, paneY, paneW, paneH, SWP_NOZORDER);

  if (g_hStatus)
    SetWindowPos(g_hStatus, NULL, kMargin, clientH - kStatusH,
                 clientW - kMargin * 2, 20, SWP_NOZORDER);
} // end LayoutControls

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg) {
    case WM_CREATE:
      CreateControls(hwnd);
      return 0;

    case WM_SIZE:
      LayoutControls(hwnd);
      return 0;

    case WM_GETMINMAXINFO:
    {
      MINMAXINFO* mmi = (MINMAXINFO*)lParam;
      mmi->ptMinTrackSize.x = 900;
      mmi->ptMinTrackSize.y = 520;
      return 0;
    }

    // ---- progress from the worker thread ----
    case WM_APP_PROGRESS:
    {
      int percent = (int)wParam;

      SendMessageW(g_hProgress, PBM_SETPOS, (WPARAM)percent, 0);

      wchar_t pct[32];
      swprintf(pct, 32, L"%d%%", percent);
      SetWindowTextW(g_hPercent, pct);

      wchar_t buf[160];
      swprintf(buf, 160, L"正在比较... %d%%    已处理 %s 字节",
               percent, FormatNum((unsigned long long)(DWORD)lParam).c_str());
      SetWindowTextW(g_hStatus, buf);
      return 0;
    }

    case WM_APP_DONE:
      FinishCompare();
      return 0;

    case WM_NOTIFY:
    {
      NMHDR* nh = (NMHDR*)lParam;
      if ((nh->code == EN_VSCROLL || nh->code == EN_HSCROLL) &&
          (nh->hwndFrom == g_hPane1 || nh->hwndFrom == g_hPane2)) {
        SyncScroll(nh->hwndFrom);
      }
      return 0;
    }

    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case IDC_FILE1_BROWSE:
          BrowseForFile(hwnd, g_hFile1Edit);
          return 0;

        case IDC_FILE2_BROWSE:
          BrowseForFile(hwnd, g_hFile2Edit);
          return 0;

        case IDC_COMPARE_BTN:
          if (g_thread) {
            InterlockedExchange(&g_cancel, 1);
            SetWindowTextW(g_hCompareBtn, L"取消中...");
          } else {
            OnCompare(hwnd);
          }
          return 0;

        case IDC_SWAP_BTN:
        {
          std::wstring a = GetCtlText(g_hFile1Edit);
          std::wstring b = GetCtlText(g_hFile2Edit);
          SetWindowTextW(g_hFile1Edit, b.c_str());
          SetWindowTextW(g_hFile2Edit, a.c_str());
          return 0;
        }

        case IDC_CLEAR_BTN:
          SetWindowTextW(g_hPane1, L"");
          SetWindowTextW(g_hPane2, L"");
          SetWindowTextW(g_hLabel1, L"文件 1");
          SetWindowTextW(g_hLabel2, L"文件 2");
          SetWindowTextW(g_hStatus, L"");
          SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
          SetWindowTextW(g_hPercent, L"");
          return 0;
      }
      break;

    case WM_CTLCOLORSTATIC:
    {
      HDC hdc = (HDC)wParam;
      SetBkMode(hdc, TRANSPARENT);
      return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }

    case WM_CLOSE:
      // Let a running comparison stop before the window goes away.
      if (g_thread) {
        InterlockedExchange(&g_cancel, 1);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
        delete g_job;
        g_job = NULL;
      }
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
} // end WndProc

//====================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
  g_hInst = hInstance;

  // RichEdit 4.1 (Msftedit.dll) gives us a much larger text limit.
  LoadLibraryW(L"Msftedit.dll");

  INITCOMMONCONTROLSEX icc;
  icc.dwSize = sizeof(icc);
  icc.dwICC  = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc;
  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize        = sizeof(wc);
  wc.lpfnWndProc   = WndProc;
  wc.hInstance     = hInstance;
  wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = L"VBindiffGuiWnd";
  wc.hIcon         = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
  wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                       IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON),
                                       GetSystemMetrics(SM_CYSMICON), LR_SHARED);
  if (!wc.hIcon)   wc.hIcon   = LoadIcon(NULL, IDI_APPLICATION);
  if (!wc.hIconSm) wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

  if (!RegisterClassExW(&wc)) return 1;

  g_hWnd = CreateWindowExW(0, wc.lpszClassName, L"Visual Binary Diff - GUI",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1100, 740,
                           NULL, NULL, hInstance, NULL);
  if (!g_hWnd) return 1;

  // Command line: vbindiffgui.exe [file1 [file2]]  (also useful for
  // "Send to" / drag & drop onto the exe).
  {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
      if (argc >= 2 && g_hFile1Edit) SetWindowTextW(g_hFile1Edit, argv[1]);
      if (argc >= 3 && g_hFile2Edit) SetWindowTextW(g_hFile2Edit, argv[2]);
      LocalFree(argv);
    }
  }

  ShowWindow(g_hWnd, nCmdShow);
  UpdateWindow(g_hWnd);

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (!IsDialogMessageW(g_hWnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  if (g_hFontUi) DeleteObject(g_hFontUi);

  return (int)msg.wParam;
} // end wWinMain
