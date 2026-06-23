#include "G3DTextInputContext.h"

#ifdef _WIN32

#include "F3DLog.h"

#include <windows.h>

#include <imm.h>

#include <imgui.h>

#include <cstdio>
#include <string>

namespace
{
// --- Focus / IME enable state (only touch the OS when it actually changes) --------------------
HWND gLastHwnd = nullptr;
bool gLastEnabled = false;
bool gInitialized = false;

// --- DBCS reassembly for the plain WM_CHAR path (ASCII / non-IME input) -----------------------
unsigned char gPendingLeadByte = 0;

// --- Inline composition (preedit) state -------------------------------------------------------
WNDPROC gPrevWndProc = nullptr; // VTK's original window proc, called for everything we don't handle
HWND gSubclassedHwnd = nullptr;
std::string gPreeditUtf8; // text currently being composed; empty when not composing
float gCaretX = -1.f;     // where to draw the preedit (== ImGui's IME caret), in window pixels
float gCaretY = -1.f;

std::string WideToUtf8(const wchar_t* w, int wlen)
{
  if (wlen <= 0)
  {
    return std::string();
  }
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, wlen, s.data(), n, nullptr, nullptr);
  return s;
}

// Push the committed (result) string into ImGui as proper Unicode, bypassing VTK's lossy char path.
void FeedResultString(HWND hwnd)
{
  if (ImGui::GetCurrentContext() == nullptr)
  {
    return;
  }
  HIMC himc = ::ImmGetContext(hwnd);
  if (himc == nullptr)
  {
    return;
  }
  const LONG bytes = ::ImmGetCompositionStringW(himc, GCS_RESULTSTR, nullptr, 0);
  if (bytes > 0)
  {
    std::wstring w(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
    ::ImmGetCompositionStringW(himc, GCS_RESULTSTR, w.data(), static_cast<DWORD>(bytes));
    ImGuiIO& io = ImGui::GetIO();

    // TEMP diagnostic (IME "?" investigation): log the committed codepoints so we can tell whether a
    // '?' is a wrong codepoint (delivery bug) or a correct codepoint missing from the CJK font range
    // (coverage). Goes to the file log at DEBUG. Remove once the '?' cause is confirmed.
    std::string dbg;
    for (wchar_t c : w)
    {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "U+%04X ", static_cast<unsigned>(static_cast<ImWchar16>(c)));
      dbg += hex;
    }
    F3DLog::Print(F3DLog::Severity::Debug, "IME commit codepoints: " + dbg);

    for (wchar_t c : w)
    {
      io.AddInputCharacterUTF16(static_cast<ImWchar16>(c)); // handles surrogate pairs
    }
  }
  ::ImmReleaseContext(hwnd, himc);
}

// Snapshot the in-progress composition string so the focused widget can draw it inline.
void UpdatePreedit(HWND hwnd)
{
  HIMC himc = ::ImmGetContext(hwnd);
  if (himc == nullptr)
  {
    gPreeditUtf8.clear();
    return;
  }
  const LONG bytes = ::ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
  if (bytes > 0)
  {
    std::wstring w(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
    ::ImmGetCompositionStringW(himc, GCS_COMPSTR, w.data(), static_cast<DWORD>(bytes));
    gPreeditUtf8 = WideToUtf8(w.c_str(), static_cast<int>(w.size()));
  }
  else
  {
    gPreeditUtf8.clear();
  }
  ::ImmReleaseContext(hwnd, himc);
}

// Subclass proc: take over IME composition so we can draw it inline and suppress the OS composition
// window. Everything else falls through to VTK's proc unchanged.
LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  switch (msg)
  {
    case WM_IME_STARTCOMPOSITION:
      // Swallow so the OS does not open its own (overlapping) composition window.
      gPreeditUtf8.clear();
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_IME_COMPOSITION:
      if (lparam & GCS_RESULTSTR)
      {
        FeedResultString(hwnd);
        gPreeditUtf8.clear();
      }
      if (lparam & GCS_COMPSTR)
      {
        UpdatePreedit(hwnd);
      }
      // Consume: prevents the OS composition window and the lossy WM_CHAR/WM_IME_CHAR it would emit.
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_IME_ENDCOMPOSITION:
      gPreeditUtf8.clear();
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    default:
      break;
  }
  return ::CallWindowProcA(gPrevWndProc, hwnd, msg, wparam, lparam);
}

void EnsureSubclassed(HWND hwnd)
{
  if (gSubclassedHwnd == hwnd)
  {
    return;
  }
  // VTK runs an ANSI window, so subclass with the ANSI variants to keep WM_CHAR (DBCS) flowing to
  // VTK unchanged; IME composition is read via the explicit wide Imm* calls above.
  gPrevWndProc = reinterpret_cast<WNDPROC>(
    ::SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)));
  gSubclassedHwnd = hwnd;
}
} // namespace

namespace G3DTextInputContext
{
void Update(void* nativeWindowHandle, bool wantTextInput, float caretX, float caretY)
{
  HWND hwnd = static_cast<HWND>(nativeWindowHandle);
  if (hwnd == nullptr)
  {
    return;
  }
  EnsureSubclassed(hwnd);

  if (!gInitialized || hwnd != gLastHwnd || wantTextInput != gLastEnabled)
  {
    // IACE_DEFAULT re-associates the window's default IME context (enable); a NULL context with no
    // flags disassociates it (disable), so the IME never swallows bare keystrokes meant as
    // shortcuts. The OS default is enabled, hence we disable on the first frame with no text focus.
    ::ImmAssociateContextEx(hwnd, nullptr, wantTextInput ? IACE_DEFAULT : 0);
    gLastHwnd = hwnd;
    gLastEnabled = wantTextInput;
    gInitialized = true;
  }

  if (wantTextInput && caretX >= 0.f && caretY >= 0.f)
  {
    gCaretX = caretX;
    gCaretY = caretY;
    // Glue the candidate window to the caret (the composition string itself is drawn inline by us).
    if (HIMC himc = ::ImmGetContext(hwnd))
    {
      COMPOSITIONFORM cf;
      cf.dwStyle = CFS_POINT;
      cf.ptCurrentPos.x = static_cast<LONG>(caretX);
      cf.ptCurrentPos.y = static_cast<LONG>(caretY);
      ::ImmSetCompositionWindow(himc, &cf);
      ::ImmReleaseContext(hwnd, himc);
    }
  }
}

unsigned int DecodeCharByte(unsigned int byte)
{
  char mb[2];
  int mbLen;
  const unsigned char b = static_cast<unsigned char>(byte & 0xFF);
  if (gPendingLeadByte != 0)
  {
    mb[0] = static_cast<char>(gPendingLeadByte);
    mb[1] = static_cast<char>(b);
    mbLen = 2;
    gPendingLeadByte = 0;
  }
  else if (::IsDBCSLeadByteEx(CP_ACP, b))
  {
    gPendingLeadByte = b; // hold the lead byte until the trailing byte arrives
    return 0;
  }
  else
  {
    mb[0] = static_cast<char>(b);
    mbLen = 1;
  }

  wchar_t wc[2] = { 0, 0 };
  if (::MultiByteToWideChar(CP_ACP, 0, mb, mbLen, wc, 2) >= 1)
  {
    return static_cast<unsigned int>(wc[0]);
  }
  return 0;
}

const char* CurrentPreedit()
{
  return gPreeditUtf8.c_str();
}

void CurrentCaret(float& x, float& y)
{
  x = gCaretX;
  y = gCaretY;
}
}

#else

namespace G3DTextInputContext
{
void Update(void* /*nativeWindowHandle*/, bool /*wantTextInput*/, float /*caretX*/, float /*caretY*/)
{
  // Browser / X11 / macOS scope the IME to the focused widget natively — nothing to do here.
}

unsigned int DecodeCharByte(unsigned int byte)
{
  // Non-Win32 backends already deliver a usable codepoint per CharEvent.
  return byte;
}

const char* CurrentPreedit()
{
  return "";
}

void CurrentCaret(float& x, float& y)
{
  x = -1.f;
  y = -1.f;
}
}

#endif
