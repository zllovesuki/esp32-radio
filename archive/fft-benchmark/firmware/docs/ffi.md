# Frozen FFT harness: native boundary

This contract describes the archived benchmark harness. The maintained
application has its own [native boundary contract](../../../../firmware/docs/ffi.md).

The private ABI is declared in `platform/include/radio_bridge.h` and imported
only by `esp32-radio/src/platform/ffi.rs`. The boundary carries fixed-width
integers, C strings, opaque handles and pointer/length pairs. It carries no Rust
structs, trait objects, owned strings, enums, references or allocator-owned
containers. `size_t` corresponds to Rust `usize`; both are 32-bit on this target.

All callers must provide valid pointer/length pairs, initialized inputs and
writable output storage of the declared capacity. Each Rust wrapper constructs
these pairs from live slices/strings and checks returned lengths. C performs
the copy within capacity. The C header is private, not a general-purpose public
FFI library that accepts arbitrary hostile pointers.

## Ownership and calls

| Resource                                | Owner and lifetime                                                                                   | Synchronization                                                                         |
| --------------------------------------- | ---------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| Board drivers                           | One initialized board; C retains driver handles                                                      | LED capability moves to the radio task; it is not `Sync`                                |
| Peer handle/configuration/labels/answer | C allocation retained until reset, including failed initialization once vendor code has been entered | Rust peer is neither `Send` nor `Sync`; all methods run on its creating task            |
| Vendor command payload                  | Valid only during the C callback; copied immediately                                                 | FreeRTOS queue of 16 commands, each with up to 512 payload bytes; no Rust callback      |
| Vendor offer payload                    | Copied before callback returns                                                                       | C mutex protects the stored offer and its consumer                                      |
| Peer state and dropped count            | C callback context retained until reset                                                              | C atomics                                                                               |
| Music bytes                             | C allocates and transfers initialized PSRAM to a Rust owner                                          | Immutable borrow tied to owner lifetime; C frees after borrows end                      |
| Outgoing audio/data                     | Rust borrows bytes only during the adapter call; C copies into its own bounded storage               | Sole radio task; native packet/cache copies complete before return                      |
| HTTPS client/configuration/response     | C allocates; Rust owns the opaque handle and calls C cleanup in `Drop`                               | Client is neither `Send` nor `Sync`; blocking operations stay on signaling              |
| HTTPS POST body                         | Borrowed for one blocking call                                                                       | Native POST field is cleared before return; callbacks use only C-owned response storage |
| Recovery count in RTC memory            | C retains across software resets                                                                     | C critical section; Rust admits one terminal recovery caller                            |

The HTTPS response is capped at 24,576 bytes, setup requests at 20,000 bytes,
SDP at 16,000 bytes, outbound data at 1,024 bytes, and individual Opus packets
at 1,275 bytes. Redirects are disabled. Time synchronization precedes verified
TLS, using the ESP-IDF certificate bundle. Credentials are compiled into the
private C configuration and never formatted into Rust errors.

## Vendor lifetime requirements

The adapter keeps configuration, callback context, labels and remote SDP alive
until reset. Audio/data sends copy into C-owned buffers, and the pinned native
implementation copies packets into its transport storage before returning.
Review that assumption when upgrading the vendor binary; do not infer a new
lifetime guarantee merely from an unchanged function signature.

## Failure and teardown

Release builds use `panic = "abort"`. Rust panics cannot unwind through C.
Expected input and transport failures use results/status codes; malformed
commands are rejected and counted without restarting the device.

Peer configuration and callback memory are C-owned for the process lifetime;
the Rust wrapper does not close or free them in `Drop`. Terminal recovery resets
the complete device. See [archive transport notes](../../../README.md#native-transport) for the
native teardown issue and the conditions for changing this policy.

HTTP cleanup is different: its calls are blocking and their callbacks have
returned before the Rust method returns. C cleanup closes the client before
freeing its configuration/response context.
