# rocky-kiosk-browser

Qt WebEngine based kiosk browser for Rocky Linux.

## Features

- Fullscreen browser window
- Waiting screen until connectivity checks succeed
- ICMP / TCP / HTTP connectivity checks
- Home / Back / Go controls
- Optional URL bar
- Optional context menu disable
- Optional auto-home timeout
- Domain allow / deny policy via JSON
- Page rotation via JSON
- Locale-aware waiting screen
- Detailed logging modes
- Basic application-side shortcut suppression
- External non-HTTP/HTTPS schemes blocked
- New tab / new window requests redirected into the current window
- Fullscreen API requests ignored
- Optional User-Agent override

## Build

### Requirements

- cmake
- gcc-c++
- qt6-qtbase-devel
- qt6-qtwebengine-devel

### Build steps

```bash
cmake -S . -B build
cmake --build build
```

## Run example

```bash
./build/kiosk-browser-poc \
  --homepage=https://www.google.com \
  --check-type=http \
  --check-target=https://www.google.com \
  --urlbox \
  --homebutton \
  --backbutton \
  --disable-context-menu \
  --user-agent="Mozilla/5.0 kiosk-browser" \
  --toolbar-height=64 \
  --page-list-json=./page-list.json \
  --domain-policy-json=./domain-policy.json \
  --log-dir=$HOME/.local/log/make-rocky-bootable \
  --log-mode=debug
```

## Navigation behavior

- `Home`, `Back`, `URL`, and `Go` are only added to the toolbar when their flags are enabled
- `Go` is shown only when the URL box is enabled
- links requesting a new tab or window are redirected into the current window
- only `http` and `https` URLs are allowed for page navigation
- external schemes such as `mailto:`, `tel:`, `intent:`, and `file:` are blocked
- fullscreen requests from web content are ignored
- PDF rendering inside the browser remains allowed
- explicit domain-policy blocks return to the homepage after 10 seconds
- page load failures also return to the homepage after 10 seconds

## JSON configuration

### `domain-policy.json`

Controls which domains the browser can access.

```json
{
  "allow": [],
  "deny": []
}
```

- `deny` rules are checked first
- if `allow` is empty, all domains are allowed unless denied
- if `allow` contains entries, domains not matched by `allow` are blocked
- `*.example.com` matches subdomains such as `www.example.com`
- `example.com` and `*.example.com` should both be listed if both root and subdomains must be allowed

See `domain-policy.json.example` for a multi-rule example.

### `page-list.json`

Controls automatic page rotation.

```json
{
  "default_duration_sec": 60,
  "pages": []
}
```

- `pages` empty means page rotation is disabled
- each page entry must contain `url`
- `duration_sec` is optional per page
- if a page omits `duration_sec`, `default_duration_sec` is used
- if page rotation is enabled, the browser loops through pages in order

See `page-list.json.example` for a sample rotation list.

The waiting screen is embedded in the application and switches language by runtime locale.

- Japanese locale shows Japanese text
- other locales show English text
