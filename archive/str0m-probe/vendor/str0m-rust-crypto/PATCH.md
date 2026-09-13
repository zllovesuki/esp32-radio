# RustCrypto provider patch

Source: crates.io `str0m-rust-crypto` **0.6.0**, upstream revision
`9b159720be773693dd561dec0fb20c6d7e4aa323` from
[algesten/str0m](https://github.com/algesten/str0m). The source is used under its
MIT option; the upstream copyright and license are in `LICENSE-MIT.txt`.
`Cargo.toml.orig` preserves the published, unnormalized manifest.

The archived probe workspace selects this copy through `[patch.crates-io]`.
It makes two changes:

1. Move `dimpl/rcgen` behind the default-enabled `generate-cert` feature.
   With that feature disabled, `generate_certificate()` returns `None` and
   callers must supply a certificate through `RtcConfig::set_dtls_cert`.
2. Explicitly select dimpl's RustCrypto provider when constructing DTLS.

The probe disables default features and supplies a host-generated ECDSA P-256
certificate. The maintained firmware generates certificates on the ESP32 using
mbedTLS. The upstream certificate generator enables AWS-LC, whose C headers
reject Xtensa. Disabling it avoids that native dependency; fingerprint
verification and normal DTLS authentication remain enabled.

`provider.patch` records the diff against the published crate. Keep changes
confined to this purpose. When upgrading, compare upstream first and remove
the patch when equivalent feature controls and provider selection are available.
Check the selected dependency graph, rebuild the archived host publisher and
rerun its SFU interoperability probe.
