# Flow 0.4

A standalone PSP browser that lays pages out on the PSP itself, with CSS boxes,
tables, form fields, pictures and a pointer you move with the analog stick. It
runs JavaScript with QuickJS and fetches pages over verified TLS 1.2 HTTPS. No
proxy or remote rendering service is involved.

This repository is the browser's source. Until version 0.3.1 it was ARK
Browser, part of [FasterARK powerup](https://github.com/KKaramaligkas/FasterARK_powerup);
it was split from there with its history so that it can be developed on its
own. FasterARK ships it in its Full package and its store.

## Install

Extract `Flow.zip` to your PSP storage root so that the app is at
`PSP/GAME/Flow/EBOOT.PBP`, alongside `cacert.pem`. Keep an existing
`downloads/` folder. Launch Flow from Game. On PSP Go choose the storage
for the app; on Vita/Adrenaline use your configured `pspemu` folder.

Coming from ARK Browser: Flow keeps its downloads, cookies and cache in its own
folder, `PSP/GAME/Flow`. When the Plugin Manager updates ARK Browser to Flow, it
removes the old app, and Flow's first start moves your downloads and cookies
from `PSP/GAME/ARKBrowser` into its own folder and deletes the old folder, so
you stay signed in to your sites. A download whose name Flow already has gets a
number added (`file.zip.1`). Files you added to the old folder yourself, its
cookies if Flow already has its own, and a download that doesn't fit on the
memory stick twice stay there. If you install Flow from the zip
while ARK Browser is still installed, both apps stay until you delete
`PSP/GAME/ARKBrowser`.

The Full FasterARK package includes the browser. Plugin Manager installs its
entry after the matching release is published. A draft PR artifact is installed
manually; it is not available through the live store. The ARK updater does not
install the browser automatically. This application does not flash firmware.

The user reported that 0.1 works on PSP-3000 with CERN and CNN Lite. That report
covers the original browser. This 0.4 build was tested in PPSSPP against local
test pages (search, sign-in and article pages, and one with every kind of
picture) and against saved copies of twelve real sites (pkg.go.dev,
rubygems.org, nodejs.org, gitlab.com, hub.docker.com, jsr.io, packagist.org,
nuget.org, bitbucket.org, ubuntu.com, apache.org and pypi.org), served with
their stylesheets, scripts and, for apache.org, rubygems.org and packagist.org,
their pictures from a local HTTPS server. It still needs testing on a real PSP
and over live connections.
Pages that JavaScript builds entirely, such as GitLab's, show little.

## Controls

Confirm (✕ by default) and Cancel follow the system button preference.

| Button | Page view |
| --- | --- |
| Analog stick | Move the pointer; push it past the top or bottom edge to scroll |
| Confirm | Click what the pointer is on: open a link, type into a field, tick a box, choose an option, press a button |
| L / R | Jump the pointer to the previous / next link or field |
| Up / Down | Scroll |
| Left / Right | Scroll a screen at a time |
| Triangle | Enter an address, or words to search for |
| Cancel | Back, or cancel loading |
| Square | Download the link under the pointer, or the current page |
| Select | Switch to the reader view |
| Start | Menu: address, back, forward, reload, link list, reader view, download, home, Wi-Fi, JavaScript, clear cookies, exit |

The status bar shows where the link under the pointer goes, or what Confirm
will do to the field under it. In a drop-down list, Up/Down choose, Confirm
picks and Cancel closes it.

The reader view (Select) shows the page as plain text with numbered links: L/R
select a link, Confirm opens it, Up/Down scroll, Left/Right scroll a screen,
and Select returns to the page view. The link list (Start menu) lists the
page's links, up to 256.

Words typed at Triangle that aren't an address (with spaces, or no dot) are
searched on DuckDuckGo Lite, which works without JavaScript. An address without
a scheme uses HTTPS. JavaScript is enabled by default; toggle it from Start and
reload to apply. Failed or cancelled navigation keeps the current page and
history. Back reloads a previous address. Downloads go into `downloads/`, keep
existing files, and can resume when retried if the server supports it. They
never execute or install automatically. After a page fails, Square downloads
that address.

## Page view

Pages are laid out like a desktop browser's normal flow, scaled to the PSP's
480x272 screen (one CSS pixel is 0.6 PSP pixels). Blocks stack with their
margins, padding, borders, backgrounds and widths; text wraps at spaces with
bold, sizes, colors, underlines, strike-through and text-transform; lists get
bullets or numbers; tables share their width between columns by content, with
`colspan`, cell padding, spacing and borders. An inline-block holding blocks
(a menu or a card) becomes a box as wide as its content.

Side-by-side layouts are laid out as on a computer. A flex container puts its
items in a row, where they grow, shrink and wrap as `flex`, `flex-wrap`,
`order` and `min-width` say. Auto margins, `justify-content` and `gap` space
them, and `align-items` stretches them to the row's height or aligns them in
it. A column flex container stacks its items, centered when `align-items`
says so. A grid places its items in the columns of `grid-template-columns`
(lengths, `fr`, `repeat()`, `minmax()` and `auto-fill`), spanning them with
`grid-column`. A floated box sits at the left or right with the following text
beside it, `clear` moves content below floats, and a block grows to hold the
floats in it. The screen doesn't scroll sideways, so items that wouldn't fit
even at their narrowest wrap to the next row, and grid columns narrower than a
few words are merged. `position` is ignored: positioned boxes are laid out in
the normal flow, after a flex or grid container's items.

Text hidden for screen readers, `display:none`, `visibility:hidden` and
off-screen text are not shown. Italic text is drawn upright, and CSS font
families are not used. HTML 4's named characters (`&rsaquo;`, `&eacute;`,
`&rarr;`...) and HTML 5's common ones are decoded; soft hyphens and zero-width
characters are dropped. A link or button showing only an icon shows its name
(`aria-label`, `title` or the picture's description) instead, and a link's
picture once it has arrived. Links that touch, as in menus spaced out by CSS,
are kept a space apart.

## Pictures

JPEG, PNG and GIF pictures load after the page is shown, nearest to the screen
first, and appear as they arrive. Each is decoded at the size it's shown at,
never larger (a large JPEG at a half, a quarter or an eighth of its size), and
kept in 16 bits a pixel, or 32 when it has transparent parts. `srcset` gives
the smallest candidate wide enough for its box (or the 1x one), `<picture>` its
first source in one of those formats whose `media` matches, and lazy-loading
pages their `data-src` and `data-srcset`. A picture sized by the page shows in
its box at once; one sized by itself shows its description until its size is
known, and then the page is laid out again. The text you're reading stays where
it is and what you typed into fields stays. GIFs show their first frame. WebP,
AVIF and SVG pictures and CSS background images aren't shown; their description
is. The status bar counts the pictures still to load.

A page keeps up to 160 pictures, 4 MB of them decoded (12 MB on PSP-2000 and
later), each from a download of at most 1.5 MB or a `data:` address of 32 KB.
Past the budget a picture is decoded smaller, then not at all. A progressive
JPEG is held whole while it decodes and isn't shown when that takes over 6 MB;
a PNG over 4 megapixels isn't either, and every picture is decoded only with
3 MB left over for the rest of the browser. Opening another page frees the
current page's pictures first.

## Forms and cookies

Text, password and multi-line fields open the PSP's on-screen keyboard.
Checkboxes, radio buttons, drop-down lists, and submit and reset buttons work as
on a computer, and labels point to their fields. Forms are sent with GET or POST
(`application/x-www-form-urlencoded`); a form with one text field, such as a
search box, is sent when you finish typing. Forms that upload files send only
their text fields. Buttons handled by JavaScript don't respond, because scripts
run only while the page loads.

Cookies are kept in `cookies.txt` next to the app, so sites can keep you signed
in. They are sent with page, stylesheet, script and `fetch` requests. Clear
them from the Start menu.

Some sites need more than this browser offers: Google, for example, may ask
you to turn on JavaScript instead of showing results. DuckDuckGo Lite works.

## PPSSPP

Flow runs in PPSSPP. Recent PPSSPP versions have the PSP's fonts only
when a PSP firmware is installed in them; without the fonts the browser uses a
basic built-in font and shows a notice when it starts. To get the normal fonts,
install a firmware in PPSSPP ("Install PSP firmware update" on a game disc's
info screen). Wi-Fi in PPSSPP uses the computer's internet connection.

## JavaScript

QuickJS 2026-06-04 supports modern language syntax including `let`/`const`, arrow
functions, classes, destructuring, template strings, BigInt, promises and
async/await. Inline and same-origin external scripts and ES modules run during
loading, with bounded same-origin module imports and promise jobs.

The reader DOM implements element, text, comment and fragment creation,
querySelector(All), matches and closest, ID, tag, class and name queries,
parent/child/sibling navigation, textContent, innerText, a limited innerHTML
fragment parser, insertAdjacentHTML, append/prepend/before/after/remove/replace,
cloneNode, attributes, dataset, classList and inline style changes, plus
`document.documentElement`, `head` and `body`. DOMContentLoaded and load
handlers run before the page is laid out; a handler that throws doesn't stop
the others. `fetch` and `XMLHttpRequest` support same-origin GET with text/JSON
responses. Requests and redirects must remain same-origin; they carry the
browser's cookies, but `document.cookie` reads as empty, and authorization and
custom headers are absent. `URL`, `URLSearchParams`, `matchMedia` (answered as
the stylesheets are, for 786 CSS pixels), `performance.now`, in-memory
`localStorage`/`sessionStorage` and `CustomEvent` are available.

This is a small browser DOM, not the full Web platform. Selectors support tags,
IDs, classes, attributes (all operators), the descendant, `>`, `+` and `~`
combinators, and structural pseudo-classes (`:not()`, `:is()`, `:first-child`,
`:nth-child()` and the like, `:checked`, `:disabled`); an invalid selector throws
a SyntaxError. Node collections are snapshots. The fragment parser is
deliberately limited. Timers, intervals and animation frames each run once after
loading; requested delays are not emulated. Mutation, intersection and resize
observers exist but never report. There is no persistent event loop after
loading, click handler dispatch, script-driven form submission, WebSocket,
canvas, layout measurement (sizes read as 0), workers or OS bindings.
`location` is a read-only address snapshot. Scripts from other sites aren't
downloaded, so pages built on a CDN's library (jQuery from a CDN) or entirely by
a framework will still fail; failed scripts show available readable text.

## CSS

Inline style, style blocks and external stylesheets support the cascade:
specificity, source order and `!important`. Selectors can use tags, IDs,
classes (including escaped ones such as Tailwind's `md:flex`), attributes
(`[type=search]`, `^=`, `$=`, `*=`, `~=`, `|=`), the descendant, child and
sibling combinators, `:not()`, `:is()`, `:where()`, `:first-child`,
`:last-child`, `:nth-child()` and its relatives, `:root`, `:empty`,
`:checked`, `:disabled` and `:lang()`. States a page nobody has touched
doesn't have (`:hover`, `:focus`, `:visited`) and pseudo-elements (`::before`)
never match.

`@media` queries are evaluated for the page view, which is 786 CSS pixels wide
(a tablet: navigation bars stay expanded rather than collapsing into menus that
need scripts). Light color schemes, landscape orientation and a fine pointer
match; print styles don't. `@layer` and `@supports` blocks apply. `var()`
resolves custom properties from the element's own rules, then from the root
element's. Colors can be named, hex, `rgb()`, `hsl()`, `oklch()` or `oklab()`.

Supported properties include color, background color, font size/weight/style,
text decoration and transform, text alignment, white space, display,
visibility, list style, margin, padding, border, width, min-width, max-width,
height, float, clear, and the flex and grid properties above, in px, em, rem,
pt, %, vw, vh and ch. Old HTML attributes (`bgcolor`,
`align`, `width`, `<font>`, `<center>`) work too. Elements moved out of view
(screen-reader text, closed drawers, collapsed menus) aren't shown. A gradient
background is drawn in the average of its colors. Text that would vanish into
what is behind it (white text meant for a background picture, which isn't
drawn) is shown dark on light backgrounds and light on dark ones.
Positioning, multi-column text, animations, @import and background images are
unavailable.

## Larger pages and PSP limits

Pages are downloaded with HTTP compression into `.cache/page.tmp` on storage,
then parsed in small chunks; the complete response is never buffered in RAM.
The temporary file is deleted after use or transfer failure. A forced power-off
may leave it behind; the next load replaces it. Decoded responses are capped at
8 MB, reader text at 256 KB, links at 256, anchors at 128 and DOM nodes at 16,384.
DOM text/attributes are bounded at 768 KB and nesting at 64. Oversized script
blocks are skipped while parsing continues to later readable content. The page
view holds up to 16,384 laid-out pieces with 512 KB of text, 1,024 links, 256
form fields and 32 forms; longer pages are cut off with a notice.

Stylesheets are downloaded up to 2 MB each (4 MB in all) and reduced at once
to the rules that can match the page, which keeps up to 4,096 rules in 384 KB;
SVG drawings and `<template>` contents aren't kept either. Scripts are
limited to 512 KB each / 1 MB source total; asset requests are capped at 16.
JavaScript gets an accounting heap limit of 8 MB, a 64 KB stack limit, at most
2,048 pending jobs per drain, and a 30-second execution budget per page. Pages
of more than 6,000 DOM nodes are shown without running their scripts, which
wouldn't fit in that heap; scripts may add up to 2,048 nodes.
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

Clone with `git clone --recursive`: the
[Plugin Manager](https://github.com/KKaramaligkas/PluginManager) is a
submodule in `PluginManager/`, and the browser builds its network, text, input
and drawing code, ships its `cacert.pem` and runs its import check.

`make package` builds `dist/Flow.zip`. It needs the
[pspdev](https://github.com/pspdev/pspdev) toolchain and
`psp-pacman -S curl mbedtls cjson libintrafont zlib jpeg libpng giflib`; CI uses the same pinned
SDK inputs as FasterARK (`tools/toolchains.json`). QuickJS engine sources are
vendored and built without OS helpers or atomics; see
`vendor/quickjs/README.flow.md`.

`make -C tests check` requires a native C compiler, cJSON, libcurl, libjpeg,
libpng and giflib development packages, Python and OpenSSL. Address/undefined-behavior sanitizers
cover URL/HTML/history regressions, streaming boundaries and multi-MB scripts,
CSS cascade, DOM mutations, modern syntax, promises, GET, module and resource
failures, cancellation and infinite loops. The page view tests cover text flow
and line breaking, boxes, lists, tables, flex and grid layouts, floats, form
fields and submission, links, anchors, pointer movement and pictures' boxes. The
picture tests decode JPEGs, PNGs and GIFs the libraries make (and damaged
copies of them), choose sources, and fill the page's table within its budget.
The transport fixtures verify TLS 1.2, invalid
certificates, downgrade rejection and redirect limits.

Follow [the hardware checklist](https://github.com/KKaramaligkas/FasterARK_powerup/blob/main/docs/hardware-release-checklist.md) on a
physical PSP before treating this build as device-validated.

GPL-3.0 for ARK/reused code; QuickJS is MIT (included as QuickJS-LICENSE in the
archive). libcurl, mbedTLS, cJSON, intraFont and zlib retain their licenses.
