// webview2_abi.cpp - compile-time guard for the vendored WebView2 SDK.
//
// This translation unit exists only so that a plain `build.bat` fails loudly if
// sdk\webview2\include ever goes missing or drifts. Nothing here runs.
//
// Include order is not optional: WebView2.h is MIDL-generated and relies on
// `interface`, `DECLSPEC_UUID` and the WRL event-token glue being declared
// beforehand, so objbase.h + eventtoken.h + wrl.h come first. eventtoken.h lives
// in the SDK's winrt directory, which therefore has to be on INCLUDE.

#include <windows.h>

#include <eventtoken.h>
#include <objbase.h>
#include <wrl.h>

#include <WebView2.h>

// If this stops holding, the SDK on disk is not the one the rest of the code was
// written against (COREWEBVIEW2_COLOR is A,R,G,B - one byte per channel).
static_assert(sizeof(COREWEBVIEW2_COLOR) == 4, "unexpected COREWEBVIEW2_COLOR layout");

// Keeps the TU non-empty for the linker without exporting anything.
extern "C" int DopusMdViewerWebView2AbiMarker = 0;
