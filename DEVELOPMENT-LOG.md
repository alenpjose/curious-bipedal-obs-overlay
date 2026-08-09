# Development log

## 2026-08-09

- Synchronized the clean `agent/native-plugin-hardening` checkout from authoritative GitHub commit `4256d404ea9b960c36d24c2784faf4c3ca283b0e` using a fast-forward-only update. SSH authentication was unavailable for the desktop account, so the authoritative ref was verified and fetched over HTTPS without changing the configured remote.
- Confirmed the vertical preset callback reapplied `1440x2560` dimensions during property initialization/refresh. Began replacing this with one-time explicit preset application, a `1080x1920` vertical default, persistent per-source custom dimensions, and an explicit reapply button.
- Verified against Aitum Vertical 1.6.4 source that `aitum_vertical_get_stream_output` returns a referenced `obs_output_t`. Identified that the plugin retained a stopped predecessor when Aitum replaced its output object, preventing later retries. Began implementing once-per-second output identity/active-state reconciliation while preserving elapsed time across transient lookup gaps.
- Began hardening per-source hotkey teardown by using the existing lifetime guard in callbacks and explicitly unregistering both source hotkeys during destruction.
- Review found and removed a lock-order hazard in the reconciliation path. Added pending-stop timestamp handling so an Aitum predecessor's stop signal does not interrupt elapsed time when an active replacement appears; a genuine stop is committed at the original signal time.
- Commit `f99fa65b45ff78766e97b964c81d0f45f39e674a` passed Windows Actions run `31328566480`: OBS 32.1.2 compilation, portable archive, Inno installer, installer/package SHA-256 comparison, uninstaller cleanup, PE x64 validation, and artifact upload. Artifact inspection found two non-runtime logo variants in the portable package, so the install manifest and package test are being tightened before final delivery.

Further build, runtime, packaging, and GitHub delivery evidence will be appended below.
