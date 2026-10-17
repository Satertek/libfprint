# LibFPrint — Goodix 532D (`27c6:532d`) for Fedora Bluefin

> [!IMPORTANT]
> **Scope & Caveats:**
> 1. **Narrow Hardware & OS Focus:** This branch (`driver/532d`) is specifically engineered and tested only for the **Goodix `27c6:532d` (`GM532D`) TLS Fingerprint Sensor** (Dell XPS 13 9310 2-in-1) running on **Fedora Bluefin / Silverblue (ostree immutable Linux)**. These changes have **not** been tested on any other sensor models, hardware variants, or Linux distributions.
> 2. **Change in Authentication Algorithm:** For small-area sensor frames ($\le 96 \times 96$ px), this branch replaces `libfprint`'s standard **NIST NBIS (`mindtct` / `bozorth3`) minutiae matcher** with a custom **2D Normalized Cross-Correlation (NCC) direct ridge template matcher**. Because the $64 \times 80$ pixel sensing array ($\sim 3.2\text{ mm} \times 4.0\text{ mm}$) captures only 5–8 ridge cycles per touch, NBIS minutiae extraction is geometrically starved; the replacement 2D correlation algorithm matches preprocessed ridge topology directly across multi-stage templates. While verified to accept genuine fingers and reject impostors on this device, it has not undergone formal biometric security certification or testing across a broad population.

---

## Summary of Modifications (`driver/532d`)

### 1. Goodix `27c6:532d` Hardware & Protocol Fixes (`libfprint/drivers/goodixtls/goodix53xd.c`)
* **Device Support:** Added USB ID `27c6:532d` (`goodixtls53xd`) and `udev` autosuspend rules.
* **Synchronous NOP & Timeout Guards:** Fixed false timeouts on synchronous NOP commands and guarded asynchronous timeout callbacks during TLS handshake and state transitions.
* **Dynamic FDT Calibration:** Implemented dynamic Finger Detection (FDT) baseline calibration and automatic `touch_flag` drift re-arming so finger placement and removal events trigger reliably without hanging.
* **OTP DAC & Background Calibration:** Implemented OTP DAC calibration parsing, sensor register `0x022c` (`0x030a`) initialization, and `0x81` empty frame capture, paired with fallback support for a pre-calibrated factory baseline (`/var/lib/fprint/goodix53xd_clear.pgm`).

### 2. Capacitive Signal Conditioning (`libfprint/drivers/goodixtls/goodix53xd.c`)
* **Contact-Masked Fixed-Pattern Noise Removal:** Subtracts the per-pixel sensor baseline and removes horizontal/vertical multiplexing stripe artifacts computed strictly over active finger contact pixels (`raw < 3800`), preventing saturated air borders from skewing row/column means.
* **2D Bandpass & Local Variance Normalization:** Applies a 2D separable Gaussian bandpass filter ($\sigma_{\text{low}} = 0.5$, $\sigma_{\text{high}} = 2.5$) tuned to the native 6–8 pixel ridge wavelength, followed by local energy normalization ($\sigma_{\text{var}} = 3.0$) to equalize contact pressure across the frame.

### 3. Direct 2D Ridge Correlation Matcher (`libfprint/fpi-print.c`, `libfprint/fp-print.c`, `libfprint/fp-image.c`)
* **Why NBIS Minutiae Fails on `27c6:532d`:** The Goodix 532D has an ultra-small $64 \times 80$ pixel sensing array ($\sim 3.2\text{ mm} \times 4.0\text{ mm}$), capturing only 5 to 8 ridge cycles per touch. Standard NIST NBIS (`mindtct` / `bozorth3`) was designed for 500 DPI flat sensors ($\ge 250 \times 250$ px) and fails on sensors of this size due to a lack of interior bifurcations and dominance of border-clipped ridge endings.
* **Multi-Stage 2D Normalized Cross-Correlation (NCC):** Added a direct 2D phase-preserving ridge template matcher for small-area sensors ($\le 96 \times 96$ px). During the 10-stage enrollment, normalized ridge and contact-mask templates are collected across all 10 touches. During verification, the probe frame is compared against the enrolled gallery across translations and rotations ($\pm 24^\circ$ in $3^\circ$ steps with $1^\circ$ fine refinement), reliably accepting genuine fingers ($\text{score} \ge 36$) and rejecting impostor fingers.
* **GVariant Persistence:** Extended `FpPrint` serialization (`fp_print_serialize` / `fp_print_deserialize`) to store the multi-stage `"ridge-templates"` (`a(iiay)`) inside the `FpPrint` expansion dictionary so enrolled prints persist in `/var/lib/fprint/` across daemon restarts and reboots.

### 4. Bluefin / Silverblue Automated Installer (`install-bluefin.sh`)
* **Host & Container Aware:** Automatically builds `libfprint` using a Distrobox `bench` container (or native `ninja`) and installs the shared library on the immutable host under `/usr/local/lib64`.
* **Systemd & PAM Integration:** Configures `/etc/ld.so.conf.d/00-libfprint-local.conf`, adds a `systemd` drop-in override for `fprintd.service` (`ReadOnlyPaths=/var/usrlocal` to work with `ProtectSystem=strict` on ostree), installs Polkit authorization rules for the `wheel` group, and enables `with-fingerprint` via `authselect`.

### Quick Start on Fedora Bluefin
```bash
./install-bluefin.sh
```

---

<div align="center">

# LibFPrint

This is an experimental libfprint driver implementation for Goodix drivers.

Currently in the works:
- 27c6x5110 (80x64 resolution)
- 27c6x532d (64x80 resolution)

*LibFPrint is part of the **[FPrint][Website]** project.*

<br/>

[![Button Website]][Website]
[![Button Documentation]][Documentation]

[![Button Supported]][Supported]
[![Button Unsupported]][Unsupported]

[![Button Contribute]][Contribute]
[![Button Contributors]][Contributors]

</div>

## History

**LibFPrint** was originally developed as part of an
academic project at the **[University Of Manchester]**.

It aimed to hide the differences between consumer
fingerprint scanners and provide a single uniform
API to application developers.

## Goal

The ultimate goal of the **FPrint** project is to make
fingerprint scanners widely and easily usable under
common Linux environments.

## License

`Section 6` of the license states that for compiled works that use
this library, such works must include **LibFPrint** copyright notices
alongside the copyright notices for the other parts of the work.

**LibFPrint** includes code from **NIST's** **[NBIS]** software distribution.

We include **Bozorth3** from the **[US Export Controlled]**
distribution, which we have determined to be fine
being shipped in an open source project.

<br/>

<div align="right">

[![Badge License]][License]

</div>


<!----------------------------------------------------------------------------->

[Documentation]: https://fprint.freedesktop.org/libfprint-dev/
[Contributors]: https://gitlab.freedesktop.org/libfprint/libfprint/-/graphs/master
[Unsupported]: https://gitlab.freedesktop.org/libfprint/wiki/-/wikis/Unsupported-Devices
[Supported]: https://fprint.freedesktop.org/supported-devices.html
[Website]: https://fprint.freedesktop.org/

[Contribute]: ./HACKING.md
[License]: ./COPYING

[University Of Manchester]: https://www.manchester.ac.uk/
[US Export Controlled]: https://fprint.freedesktop.org/us-export-control.html
[NBIS]: http://fingerprint.nist.gov/NBIS/index.html


<!---------------------------------[ Badges ]---------------------------------->

[Badge License]: https://img.shields.io/badge/License-LGPL2.1-015d93.svg?style=for-the-badge&labelColor=blue


<!---------------------------------[ Buttons ]--------------------------------->

[Button Documentation]: https://img.shields.io/badge/Documentation-04ACE6?style=for-the-badge&logoColor=white&logo=BookStack
[Button Contributors]: https://img.shields.io/badge/Contributors-FF4F8B?style=for-the-badge&logoColor=white&logo=ActiGraph
[Button Unsupported]: https://img.shields.io/badge/Unsupported_Devices-EF2D5E?style=for-the-badge&logoColor=white&logo=AdBlock
[Button Contribute]: https://img.shields.io/badge/Contribute-66459B?style=for-the-badge&logoColor=white&logo=Git
[Button Supported]: https://img.shields.io/badge/Supported_Devices-428813?style=for-the-badge&logoColor=white&logo=AdGuard
[Button Website]: https://img.shields.io/badge/Homepage-3B80AE?style=for-the-badge&logoColor=white&logo=freedesktopDotOrg
