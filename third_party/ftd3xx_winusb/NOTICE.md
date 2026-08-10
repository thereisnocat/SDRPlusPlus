# FTDI D3XX WinUSB SDK (vendored)

These three files are extracted from FTDI's `WU_FTD3XXLib` folder, which is part of the
official **WinUSB D3XX driver package** for FTDI's USB3.0 SuperSpeed ICs (the FT60x family,
which is what the RSR200 uses):

- `FTD3XX.h` — API header (`WU_FTD3XXLib/Lib/FTD3XX.h`)
- `x64/FTD3XXWU.lib` — x64 import library for the dynamic-linked driver (`WU_FTD3XXLib/Lib/Dynamic/x64/FTD3XXWU.lib`)
- `x64/FTD3XXWU.dll` — x64 runtime DLL (`WU_FTD3XXLib/Lib/Dynamic/x64/FTD3XXWU.dll`)

**Source:** `https://ftdichip.com/wp-content/uploads/2025/08/Winusb_D3XX_Release_1.4.0.1.zip`,
version 1.4.0.1, downloaded 2026-08-10 from FTDI's official D3XX Drivers page
(`https://ftdichip.com/drivers/d3xx-drivers/`).

**Why vendored instead of downloaded by CI:** `ftdichip.com` sits behind Cloudflare
bot-detection that blocks non-interactive HTTP clients (confirmed against both `curl` and
GitHub Actions' `Invoke-WebRequest` pattern used by every other vendor SDK step in
`build_all.yml` — both get served the Cloudflare JS challenge page instead of the file, even
when pointed at the direct `wp-content/uploads` URL). A real, interactive browser session was
the only thing that got past it. Rather than have Windows CI depend on an unreliable
third-party download that's known to fail non-interactively, these few small files (~177KB
total) are committed directly. See `RSR200_PLAN.md` section 14 for the full investigation.

**Do not confuse this with the older WDF-based driver.** FTDI's site lists two Windows D3XX
packages: the current WinUSB one (this one, `Winusb_D3XX_Release_*.zip`, marked
"(Recommended)" and "*Latest Windows WinUSB based driver") and a separate, older,
soon-to-be-deprecated WDF-based one (`FTD3XXDriver_WHQLCertified_v*.zip`, marked "**Previous
Windows WDF based driver"). Only the WinUSB one produces `FTD3XXWU.lib`/`FTD3XXWU.dll`, which
is what `source_modules/rsr200_source/CMakeLists.txt` links against. The WDF package instead
ships a kernel-mode `ftdibus3.sys`/`.inf` and no `FTD3XXWU` import library at all.

**License:** per FTDI's D3XX Drivers page, "FTDI drivers may be used only in conjunction with
products based on FTDI parts" and "FTDI drivers may be distributed in any form as long as
license information is not modified" (full terms:
`https://ftdichip.com/driver-licence-terms/`). Nothing in these three files has been modified
from the original package.
