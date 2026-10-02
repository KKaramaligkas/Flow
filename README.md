# ARK Browser 0.2

A standalone PSP browser with on-device QuickJS, basic CSS text rendering and
direct, verified TLS 1.2 HTTPS. All parsing and JavaScript run on the PSP. No
proxy or remote rendering service is required.

## Install

Extract `ARKBrowser.zip` to your PSP storage root so that the app is at
`PSP/GAME/ARKBrowser/EBOOT.PBP`, alongside `cacert.pem`. Keep an existing
`downloads/` folder. Launch ARK Browser from Game. On PSP Go choose the storage
for the app; on Vita/Adrenaline use your configured `pspemu` folder.

The Full FasterARK package includes the browser. Plugin Manager installs its
entry after the matching release is published. A draft PR artifact is installed
manually; it is not available through the live store. The ARK updater does not
install the browser automatically. This application does not flash firmware.

The user reported that 0.1 works on PSP-3000 with CERN and CNN Lite. That report
covers the original browser. This 0.2 build still requires physical testing;
host tests and PSP/Vita compilation do not prove modern-site compatibility.

## Controls

Confirm and Cancel follow the system button preference.

| Button | Action |
| --- | --- |
| Triangle | Address entry; a hostname without a scheme uses HTTPS |
| Up / Down | Scroll, or choose a link in the link list |
| Left / Right | Scroll a page |
| L / R | Select previous / next numbered link |
| Confirm | Open selected link, including in-page anchors |
| Cancel | Back, or cancel loading / JavaScript |
| Select | Toggle the link list |
| Square | Download selected link or current page |
| Start | Address, back, reload, download, Wi-Fi, home, JavaScript toggle, exit |

JavaScript is enabled by default. Toggle it from Start and reload to apply.
Failed or cancelled navigation retains the current page and history. Back
reloads a previous address. Downloads go into `downloads/`, keep existing files,
and can resume when retried if the server supports it. They never execute
or install automatically. After a page fails, Square downloads that address.

## JavaScript

QuickJS 2026-06-04 supports modern language syntax including `let`/`const`, arrow
functions, classes, destructuring, template strings, BigInt, promises and
async/await. Inline and same-origin external scripts and ES modules run during
loading, with bounded same-origin module imports and promise jobs.

The reader DOM implements basic element/text creation, querySelector(All), ID,
tag and class queries, textContent, innerText, a limited innerHTML fragment
parser, append/remove/insert, attributes, classList and inline style changes.
DOMContentLoaded and load handlers run before the final reader view. `fetch`
supports same-origin GET with text/JSON promise responses. Requests and redirects
must remain same-origin; cookies, authorization and custom headers are absent.

This is a small browser DOM, not the full Web platform. Selectors support tags,
IDs, classes and descendants. Node collections are snapshots. The fragment
parser is deliberately limited. One-shot function timers run once after loading;
requested delays are not emulated. There is no persistent event loop after
loading, click handler dispatch, setInterval, interactive form submission,
XHR, WebSocket, storage, canvas, layout measurement, workers or OS bindings.
`location` is a read-only address snapshot. Full SPA frameworks and most modern
interactive sites will still fail. Failed scripts show available readable text.

## CSS and reader view

Inline style, style blocks and external stylesheets support tag/ID/class,
descendant and child selectors, specificity, source order and `!important`.
Supported properties include color, simple background color, font size/weight,
underline, text alignment, white space, display and visibility. Bold firmware
fonts, variable text sizes, colors and underlines appear in the reader view.
Italic is parsed but the firmware font renderer does not draw a slant.

Screen media blocks are accepted; print blocks are ignored. Width/feature media
queries are not evaluated. Flex/grid declarations become linear blocks;
positioning, columns, box sizing, margins, CSS variables, animations, pseudo
classes, attribute selectors, @import and image backgrounds are unavailable.
Images show alt text; images, audio, video and tabs are not implemented.

## Larger pages and PSP limits

Pages are downloaded with HTTP compression into `.cache/page.tmp` on storage,
then parsed in small chunks; the complete response is never buffered in RAM.
The temporary file is deleted after use or transfer failure. A forced power-off
may leave it behind; the next load replaces it. Decoded responses are capped at
8 MB, reader text at 256 KB, links at 256, anchors at 128 and DOM nodes at 2,048.
DOM text/attributes are bounded at 768 KB and nesting at 64. Oversized script
blocks are skipped while parsing continues to later readable content.

Stylesheets are limited to 64 KB each / 128 KB total / 256 rules. Scripts are
limited to 512 KB each / 1 MB source total; asset requests are capped at 16.
JavaScript gets an accounting heap limit of 8 MB, a 64 KB stack limit, at most
2,048 pending jobs per drain, and a 30-second execution budget per page.
Network waiting does not consume that execution budget. Fetch/module requests share
8 requests / 512 KB total, with at most 256 KB per response. These limits keep
a slow or malformed page from consuming all console memory. Notices identify
shortened text, omitted assets and script failures. Downloads can save larger
files. JavaScript always runs in the cancellable worker.

## HTTPS and Wi-Fi

Keep the PSP clock correct and `cacert.pem` alongside the app. HTTPS verifies
hostnames, certificate chains and dates, requires TLS 1.2 or newer, and rejects
HTTP downgrade redirects. HTTP remains available for old sites and is labelled
unencrypted. HTTPS pages reject HTTP subresources. IPv4 and ASCII URL entry are
used; use punycode/percent encoding where needed. TLS does not change the PSP's
Wi-Fi hardware compatibility. PSP Street has no Wi-Fi.

## Build and validation

`make browser` builds `dist/ARKBrowser.zip` with the locked PSP toolchain. The
Full PSP package also includes it. QuickJS engine sources are vendored and
built without OS helpers or atomics; see `vendor/quickjs/README.ark.md`.

`make -C Browser/tests check` requires a native C compiler, cJSON and libcurl
development packages, Python and OpenSSL. Address/undefined-behavior sanitizers
cover URL/HTML/history regressions, streaming boundaries and multi-MB scripts,
CSS cascade, DOM mutations, modern syntax, promises, GET, module and resource
failures, cancellation and infinite loops. The transport fixtures verify TLS
1.2, invalid certificates, downgrade rejection and redirect limits.

Follow [the hardware checklist](../docs/hardware-release-checklist.md) on a
physical PSP before treating this build as device-validated.

GPL-3.0 for ARK/reused code; QuickJS is MIT (included as QuickJS-LICENSE in the
archive). libcurl, mbedTLS, cJSON, intraFont and zlib retain their licenses.
