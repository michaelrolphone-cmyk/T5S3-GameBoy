# Display adapter host fixture

The display and provider ABI headers are copied unchanged from Reader PR #220
commit `e0031d06f976fa6b3940f31693907318c3de93ac`:

- `sdk/driver/RiscDisplayOutputV1.h`
- `lib/NativeApps/include/T5ProviderCapabilityApi.h`

They make the behavioral test offline and reproducible. The real ELF build
still compiles against the companion Reader checkout, not these fixtures.
Arduino timing and the provider are simulated; this is not panel qualification.
