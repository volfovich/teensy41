# NAM test files

- `A2.nam`: the A2 example model of [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
  (`example_models/A2.nam`), MIT License (`LICENSE-NeuralAmpModelerCore`). An A2 `SlimmableContainer`
  with a Lite (3-channel) and a Full (8-channel) network.
- `A2_lite_ref.f32`: NeuralAmpModelerCore's output for that model's Lite network on the golden test
  signal (`dsp/nam/GoldenSignal.h`: 1 s at 48 kHz, raw 32-bit floats). The tests check `dsp::NamA2`
  against it.

- `golden_input.f32`: that test signal as generated on the PC (raw 32-bit floats). The on-board
  check (`hw_tests/nam_a2_bench.cpp`) feeds this file, because the board's math library rounds
  `sinf`/`expf` differently and the comparison needs the exact same input. The host test checks
  that `goldenSignal()` still reproduces it.

`A2.nam` and `A2_lite_ref.f32` come from the teensy-ir-loader project (`test/host/nam/`), where `make_reference.sh`
regenerates the reference from a NeuralAmpModelerCore checkout.
