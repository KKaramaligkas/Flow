# ARK Browser 0.1

A standalone PSP text browser with direct HTTPS, TLS 1.2 or newer, and certificate verification. It uses the same libcurl/mbedTLS network code, RTC clock fixes, firmware fonts, and 2D renderer as Plugin Manager. It does not require a web proxy.

## Install

Extract `ARKBrowser.zip` to your PSP's storage root. The app should be at `PSP/GAME/ARKBrowser/EBOOT.PBP`, alongside `cacert.pem`. On PSP Go use the storage you want for the app and downloads. On Vita/Adrenaline, put the `PSP` folder under your configured `pspemu` folder. Launch **ARK Browser** from the Game column.

The Full FasterARK PSP package includes it. On an existing installation, use its Plugin Manager entry or the standalone archive. The ARK updater does not install the browser automatically. Keep an existing `downloads/` folder when replacing the app.

This is a first version. PSP/Vita compilation and host tests are separate from physical-device validation; hardware and emulator testing are still pending.

## Controls

Confirm and Cancel follow the PSP's system button preference.

| Button | Action |
| --- | --- |
| Triangle | Enter an address with the system keyboard; a hostname without a scheme uses HTTPS |
| Up / Down | Scroll text, or select a link in the link list |
| Left / Right | Scroll a page at a time |
| L / R | Select the previous / next numbered link |
| Confirm | Open the selected link |
| Cancel | Go back, or cancel a running request |
| Select | Switch between page text and a list of links |
| Square | Download the selected link, or the current page when no link is selected |
| Start | Menu: address, back, reload, download, Wi-Fi, start page, exit |

After a page fails to open, Square can download that failed address. Selecting a different link clears that target. Downloads are saved under the app's `downloads/` folder. Existing files are kept; another download gets a numbered filename. An interrupted download retains its validated partial file and resumes when retried, if the server supports it. Downloads never start or install code automatically.

## What works

- Reading HTML as text, with headings, paragraphs, lists, image descriptions, and numbered links.
- Reading plain text and JSON. UTF-8 and declared ISO-8859-1 / Windows-1252 are supported; malformed text is replaced safely.
- Absolute and relative HTTP(S) links, HTML base URLs, and redirects. Relative links use the final page URL after redirects.
- Direct file downloads, progress and cancellation, free-space checks, and verified resume.
- Back navigation across the last twelve addresses. Back reloads the page from the network; a failed request keeps the current page and history.

## Limits

JavaScript, interactive forms, logins, cookies, CSS layout, images, video, tabs, and in-page anchor scrolling are not implemented. Pages that require these features may have little usable text. URL entry accepts ASCII; use punycode for international hostnames and percent encoding for non-ASCII paths. The network stack currently uses IPv4.

Responses are limited to 512 KB after decompression. Rendered text is limited to 64 KB and links to 128 per page. A notice identifies shortened text or omitted links. Large files can still be streamed with Download.

## HTTPS and Wi-Fi

Set the PSP's date and time correctly and keep `cacert.pem` alongside the app. HTTPS checks the server's identity, certificate chain, and dates. The browser has no option to turn verification off. HTTPS requests require TLS 1.2 or newer and reject redirects to plain HTTP. HTTP is available for older text sites and is labelled **HTTP - unencrypted**. Redirects to non-web schemes are rejected.

Use a network profile the PSP can connect to. TLS support does not change Wi-Fi hardware or access-point compatibility. PSP Street has no Wi-Fi. Only sites that still offer TLS 1.2 and work within this browser's text-only limits are suitable.

## Build and test

From the repository root, `make browser` produces `dist/ARKBrowser.zip`; `make` also includes it in the Full PSP package. The PSP toolchain and libraries are the same ones used for Plugin Manager. Shared object files are built separately under `Browser/shared/`.

Host tests: `make -C Browser/tests check` (a C compiler, libcurl development files, Python 3, and OpenSSL). Tests exercise URL resolution, unsafe schemes, malformed HTML, Unicode/entities, bounded pages and links, history, trusted TLS 1.2, certificate rejection, older TLS rejection, HTTPS downgrade rejection, and redirect limits. The TLS fixtures use localhost and temporary test certificates.

For physical testing, follow [the release checklist](../docs/hardware-release-checklist.md), including the browser section. The test builds are available through the PR's GitHub Actions artifacts; merging or publishing is a separate step.

GPL-3.0, consistent with the repository and reused Plugin Manager code. libcurl, mbedTLS, cJSON, intraFont, and zlib retain their respective licenses.
