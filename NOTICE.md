# Licensing and third-party components

WineTK's standalone code, scripts, tests and documentation use the GNU General
Public License version 3 supplied in [LICENSE](LICENSE), as selected for this
repository. There is no warranty; see that license for its terms.

The Wine modifications in `patches/wine/` retain Wine's licensing and existing
copyright notices. New `wineterm.drv` files explicitly use
`SPDX-License-Identifier: LGPL-2.1-or-later`. The LGPL 2.1 text is included in
[LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt). Applying a patch does not change the
license of the surrounding Wine source. The locked upstream revision is recorded
in [config/wine-source.env](config/wine-source.env).

The optional Steam bridge follows Valve Proton's native Steam client image
preservation/export redirection approach. Reference implementations:

- [Valve Wine loader](https://github.com/ValveSoftware/wine/blob/7b3ceaf8644bb5d57140903e12897efac69be16a/dlls/ntdll/loader.c).
- [Proton lsteamclient](https://github.com/ValveSoftware/Proton/tree/bc896217ffcd67f892f952b010f898fe7bd0f0a1/lsteamclient).

Wine and Proton source checkouts, installed Proton bridge binaries, Steam client
DLLs, games and downloaded content are **not distributed in this repository**.
Preparation scripts use the user's existing installations to create local copies;
the bridge records binary versions and hashes in `out/steam-bridge/provenance.json`.
Those third-party components remain subject to their respective licenses and terms.
Linux Steam performs account authentication and ownership checks.
