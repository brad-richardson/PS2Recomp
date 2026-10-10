#ifndef PS2_TLS_MODEL_H
#define PS2_TLS_MODEL_H

// TRM1 piece 4: hot thread-locals on the MTVU-GIF path use initial-exec
// TLS on ELF targets (PRF2 #4: [linker]tlsdesc_resolver_dynamic was
// ~6-7% of MTVU-GIF; LEV2 §5 row 7). initial-exec replaces each access's
// TLSDESC resolver call with a fixed TP-relative load: values, order and
// lazy per-thread init are unchanged, so this is exact with no knob.
//
// Loader constraint: initial-exec requires static TLS surplus for our
// dlopen'd libps2EntryRunner.so on Android. A violation fails fast at
// load (no silent misbehavior); the Odin boot leg is the proof. Apple
// targets keep the default model (TLV has no resolver cost and Clang's
// tls_model attribute is ELF-only).
#if defined(__ELF__) && !defined(__APPLE__) && (defined(__GNUC__) || defined(__clang__))
#define PS2X_TLS_HOT __attribute__((tls_model("initial-exec")))
#else
#define PS2X_TLS_HOT /* default TLS model */
#endif

#endif
