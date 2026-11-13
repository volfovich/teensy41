# Volfovich NAM Forge (desktop app)

The plan is [`../APP.md`](../APP.md); the link protocol is [`../protocol/PROTOCOL.md`](../protocol/PROTOCOL.md).

So far (milestone A0):

- `crates/namforge-link/` – the protocol's frames, payload fields and message numbers in Rust, with tests against the examples shared with the firmware (`../protocol/test_frames.txt`).

```
cd app/crates/namforge-link && cargo test
```

Needs Rust (https://rustup.rs). The Tauri app itself starts at milestone A1.
