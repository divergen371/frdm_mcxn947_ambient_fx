# MCXN947 Neutron for Responsive Guitar Shimmer Control

## Executive Summary

**Verdict as of November 2026: viable only under stated constraints.** Use the NPU as an attack-triggered, low-rate classifier, not as an audio processor. Keep the existing CPU envelope detector, sample processing, shimmer generation, and parameter smoothing unchanged. NXP's published audio benchmarks make this a credible prototype, but they do not establish guitar-classification accuracy or uninterrupted performance inside your particular reverb firmware. [2]

- **Right Computational Role**: N1-16 has **4 pipes x 4 INT8 MACs**, providing **4.8 GOPS at 150 MHz** -> spend that capacity on one compact inference after an attack, not on the sample stream. [5]
- **Restricted Acceleration Contract**: NXP's explicit N1-16 table supports seven listed operator families in its **INT8 PCQ** column, but not LSTM, SVDF, MUL, or SOFTMAX -> start with a small convolutional classifier and inspect the compiled partition. [5]
- **Audio Is Supported**: NXP reports **3.650 ms** for keyword spotting and **0.816 ms** for audio anomaly detection on the NPU -> these are useful feasibility anchors, not timing guarantees for the new guitar model. [2]
- **SRAM Is The Main Constraint**: The device has up to **512 KB SRAM**, or **416 KB with ECC** -> accept a model only after accounting for the existing reverb buffers and all inference workspace. [3]
- **Use The Correct Target**: The SDK explicitly maps FRDM-MCXN947 to converter target **`mcxn94x`** -> do not substitute an i.MX RT700 or other Neutron target. [9]
- **Interrupt Safety Is Conditional**: Neutron conversion can leave CPU operators in the graph -> neither NPU offload nor a short inference time alone establishes that the 48 kHz SAI deadline is safe. [4]
- **Separate Sound Character From Harmony**: Treat brightness, note density, and single-note/chord labels as proposed control features, not proof of musical key or chord identity -> keep harmonic restrictions and a conservative fallback in a deterministic policy layer.
- **Keep A CPU Baseline**: NXP's CPU-versus-NPU keyword benchmark shows substantial acceleration for a neural model, but does not compare against a few spectral rules -> ship the NPU feature only if listening tests show a worthwhile improvement. [2]


## 1. N1-16 Supports A Small, Specific INT8 Operator Set

The hardware identity is clear: the datasheet identifies **eIQ Neutron N1-16**, and NXP's MCX guide explains the **16 INT8 MAC blocks** behind the 4.8 GOPS figure. This is not evidence that every operation advertised for the wider Neutron family is accelerated on MCXN947. [3][5]

The following is the explicit N1-16 support table in NXP's device guide and AN14241. A "No" means no NPU implementation in that table, not necessarily that a complete TFLM application cannot execute the operation on the CPU. [5][12]

| Operator | Float on N1-16 | UINT8, table's PTQ column | INT8, table's PCQ column | Consequence for this feature |
|---|---|---|---|---|
| ADD | No | No | Yes | Quantized residual/additive branches are candidates |
| AVERAGE_POOL_2D | No | No | Yes | Suitable for compact feature aggregation |
| CONV_2D | No | No | Yes | Primary building block for a spectrogram CNN |
| DEPTHWISE_CONV_2D | No | No | Yes | Candidate for a small depthwise-separable CNN |
| FULLY_CONNECTED | No | No | Yes | Suitable for the final classification head |
| LOGISTIC / Sigmoid | No | No | Yes | Candidate for independent output labels |
| MAX_POOL_2D | No | No | Yes | Candidate for reducing intermediate tensors |
| UNIDIRECTIONAL_SEQUENCE_LSTM | No | No | No | Do not plan on accelerated recurrent inference |
| MUL | No | No | No | Avoid explicit multiplicative gates in the accelerated graph |
| SOFTMAX | No | No | No | Use logits plus CPU selection, or CPU softmax if needed |
| SVDF | No | No | No | Do not assume a speech/SVDF model accelerates |

**A compact CNN is a much safer starting point than an LSTM, SVDF, or gated recurrent architecture.** Operator names are only the first compatibility check; successful conversion and actual partitioning still determine what executes on the NPU. [4]

### PCQ Is Not A Synonym For Post-Training Quantization

In this particular NXP table, **PTQ means per-tensor quantized, asymmetric 8-bit**, while **PCQ means per-channel quantized, symmetric 8-bit**. Elsewhere, PTQ often means the *post-training quantization workflow*. Those are different uses of the acronym: a model quantized after training can still use per-channel weights. [5]

The practical requirement is therefore a **compatible signed INT8 model**, not just a filename containing "quantized". Do not assume that an arbitrary float or UINT8 model becomes N1-16 compatible, or that per-channel quantization applies identically to every tensor. In particular, the previously documented Glow behavior of internally converting UINT8 is not a Neutron compatibility guarantee.

NXP's converter replaces supported regions with **`NeutronGraph` custom operators** and leaves unsupported operators for CPU execution. NXP also explicitly describes Cortex-M33/CMSIS-NN execution for operators outside the hardware-optimized list. Thus audio and keyword models are supported as applications, but their acceleration depends on their topology and quantization. [4][7]

**Version qualification:** this is the confirmed, explicitly device-specific N1-16 baseline from AN14241 and NXP's MCX guide. It should not be presented as proof that every later compiler has an identical exhaustive kernel list. The delivered model's compiler output must settle any proposed extension beyond this baseline.


## 2. Millisecond Audio Inference Is Plausible, Not Deadline-Proven

NXP provides a useful concrete case: its FRDM-MCXN947 benchmark compares the same neural workloads under Cortex-M33/TFLM execution and NPU execution. The published project lists **SDK_26_03_00_FRDM-MCXN947**. [2]

| NXP benchmark workload | Cortex-M33/TFLM | NPU | Published speed ratio |
|---|---:|---:|---:|
| Audio anomaly detection | 5.302 ms | 0.816 ms | 6.50x |
| Small-vocabulary keyword spotting | 57.514 ms | 3.650 ms | 15.76x |
| Small image classification | 239.036 ms | 6.399 ms | 37.36x |
| Visual wake words | 184.935 ms | 6.568 ms | 28.16x |

The audio rows demonstrate that N1-16 is useful beyond vision. They support a **low-single-digit-millisecond inference hypothesis** for a carefully sized audio classifier, not a universal "typical guitar inference time." These published numbers do not establish performance with your reverb ISR, feature extraction, memory traffic, or chosen network. [2]

### Foreground Blocking And Interrupt Blocking Are Different

Design the initial integration around a **synchronous foreground inference**: the main-loop job submits an input and consumes its output only when inference finishes. A foreground call waiting for a result does not inherently mean that higher-priority interrupts are disabled. However, CPU fallback operators, critical sections, shared-memory traffic, and any NPU service interrupt can still affect the audio deadline.

**The exact MCXN947 Neutron driver's blocking/non-blocking entry points, callback semantics, and interrupt-masking duration are not established by the cited driver material.** Consequently, this assessment does not promise a particular asynchronous API or certify that the supplied runtime leaves every SAI deadline unaffected. The documented runtime does dispatch `NeutronGraph` operations through NeutronDriver and firmware, but that architecture alone is not an interrupt-latency specification. [4]

**Can inference run in the main loop while a 48 kHz SAI ISR continues? Architecturally, yes, conditionally.** Accept that arrangement only if the selected runtime permits the ISR to preempt foreground work and measurements show adequate worst-case headroom. At 48 kHz, the sample period is **20.83 microseconds**, calculated as 1/48,000; a block-based SAI/DMA design has a different interrupt period, determined by its block length.

Recommended scheduling is:

1. The existing CPU detector marks an attack and identifies an immutable audio snapshot.
2. A foreground job computes features and quantizes the model input.
3. Foreground inference runs without touching active audio buffers.
4. The result enters a small mailbox.
5. The audio engine adopts the next voicing and smoothly applies bloom/detune controls at its own safe boundary.

Use one outstanding job, coalesce or discard stale requests, and retain the previous safe setting when a result is late. Never invoke the interpreter from the SAI ISR.

### A 20-50 ms Window Is Not Necessarily A 20-50 ms Wait

If the input is the *recent history already in a ring buffer*, classification can start after the attack without waiting for another full window. If the desired evidence is the following 20-50 ms of note evolution, that acquisition delay is unavoidable. Budget the complete path as **window availability + feature extraction + inference + result handoff**, and decide whether the result controls the current bloom or the next event.

**Decision:** use the published millisecond benchmarks to justify a prototype, but make worst-case audio continuity, not average inference time, the acceptance criterion.


## 3. Target `mcxn94x` And Budget Workspace Separately From Weights

The documented deployment path is **trained model -> compatible quantized TFLite -> Neutron conversion for MCXN947 -> TFLM application with Neutron support**. AN14241 describes both command-line and eIQ Portal conversion, then replacement of the model data and registration of the required operators in the application. [12]

A current SDK keyword-spotting example gives the relevant command directly: [9]

```text
neutron-converter --input guitar_int8.tflite --output guitar_mcxn94x.tflite --target mcxn94x --dump-header-file-output
```

The filenames above are illustrative; **`mcxn94x` is the documented target**. The generic converter documentation also exposes `--show-targets` and operator/kernel discovery commands. Its output remains a TFLite model, containing a mixture of standard operators and NPU-specific custom operators as required. [4]

| Toolchain evidence | What it establishes | Practical interpretation |
|---|---|---|
| AN14241 historical SDK/Neutron pairings | SDK 2.13.1 / v1.0.0; SDK 2.14.0 / v1.2.0 | Preserve documented version pairings when reproducing the older application note. [12] |
| SDK keyword example | MCU SDK 26.06.00 lists default Neutron Software Version 3.1.1 | This is a documented software pairing, not proof that every standalone compiler release is interchangeable. [9] |
| SDK target mapping | FRDM-MCXN947 uses `mcxn94x` | Select that device target explicitly. [9] |
| TFLite/Neutron runtime architecture | `NeutronGraph` operations dispatch to NeutronDriver | Include the compatible runtime/custom-op support, not only a converted model array. [4] |

The useful compatibility statement is a **matched SDK, Neutron software, converter output, and target**, not "the newest eIQ compiler should work." The cited material establishes a usable MCXN947 path, but not a complete compatibility matrix for all releases available in November 2026.

### AN14241 Shows Why File Size Alone Is Misleading

AN14241 provides a concrete integration case. Its example model changes from **299 KB to 227 KB** after conversion, while the tensor arena changes from **157 KB to 144 KB**. The reported inference takes approximately **9 ms**; the shown **98% score is an example confidence output, not a demonstrated guitar accuracy result**. [12]

That case matters because a modest-looking model file can still consume a large fraction of an MCU's SRAM. The MCXN947 datasheet specifies up to **2 MB flash**, **512 KB SRAM**, or **416 KB with ECC**. The NPU's **2 KB cache** is not a private activation memory that eliminates the application RAM budget. [3]

For your firmware, the relevant inequality is:

```text
remaining SRAM >= tensor arena + additional driver/model workspace
                  + audio snapshot + features + stack/heap margin
```

Do not double-count scratch buffers already included in the reported arena, but do not assume the arena includes every driver allocation either. Keep immutable model data in flash where the selected deployment permits; inspect the actual map and runtime allocations for copies into RAM.

| Resource | Illustrative calculation or constraint |
|---|---|
| 20 ms mono snapshot, 48 kHz, signed 16-bit | 960 samples = 1,920 bytes |
| 50 ms mono snapshot, same format | 2,400 samples = 4,800 bytes |
| Float32 snapshot instead | Twice those snapshot byte counts |
| INT8 weight storage | Approximately one byte per weight, plus scales, metadata, and compiled representation |
| Tensor arena / intermediate activations | Model-dependent; measure the converted graph, not just parameter count |
| Existing delay and reverb state | Must remain reserved before allocating ML workspace |

These snapshot figures are calculations, not NXP benchmark measurements. They show that capture history is comparatively manageable; intermediate feature maps and existing reverb state are the larger sizing risks.

There is **no defensible universal maximum TFLite file size** for this application. As an initial engineering target, try an arena in the **tens of KB**, explicitly not an NXP guarantee. If the application cannot spare that space plus features and headroom, use a much smaller model or keep the CPU rule.


## 4. Classify Playing Texture, Then Apply A Harmonic Policy

A small CNN is technically credible because CONV_2D, DEPTHWISE_CONV_2D, pooling, and FULLY_CONNECTED appear in the N1-16 INT8 support table. NXP's SDK also supplies a keyword-spotting example recognizing a static WAV file, and its benchmark includes both keyword spotting and audio anomaly detection. That is evidence of audio inference capability, not evidence that a pretrained model already solves guitar voicing selection. [5][2]

A proposed input is a short, level-normalized spectral representation of the captured guitar window: for example, a compact log-band-energy or log-mel time-frequency patch. Compute that representation on the CPU, with any suitable DSP acceleration considered separately. Quantize only the resulting classifier input according to the trained model's calibration.

| Proposed prediction | Appropriate interpretation | Important failure case |
|---|---|---|
| Bright / neutral / dark | Spectral character for choosing a control preset | Pickup position, distortion, and playing technique can dominate chord identity |
| Single note / chord / uncertain | A learned texture label | A harmonic-rich single note can resemble several spectral components |
| Sparse / medium / dense | Broad spectral occupancy | Noise, distortion, and reverb tails can inflate apparent density |
| One of six tension preferences | A learned or curated musical control choice | Acoustically similar windows can call for different tensions in different keys |
| Bloom and detune preset | A bounded control selection | An aggressive label transition can sound worse than a stable conservative default |

These are **candidate tasks to validate**, not established accuracy claims. No cited NXP result measures guitar brightness, polyphony, spectral-density classification, or musical preference on this window length.

### Proposed Case: A Bright Chord Should Not Automatically Mean #11

Consider an attack window that the classifier labels "bright, chord, dense." That description may be useful for selecting a gentler detune and a different bloom preset, but it does not establish the chord root, key, or whether #11 is harmonically appropriate. The same spectral texture can occur in different musical contexts.

For that reason, prefer **a small number of acoustic outputs followed by a deterministic control policy**. The policy can restrict choices to the player's selected key/mode or allowed tension set, then choose among **add9, sus11, 6/9, m7, maj7, and #11**. If no harmonic context is available, describe the result honestly as an aesthetic preference rather than a reliable chord-aware harmony decision.

A direct six-class tension classifier is also possible as a proposed architecture, but needs meaningful training labels: actual player preferences or a clearly specified musical policy. Brightness labels alone do not supply supervision for six distinct harmonic choices. Likewise, bloom and detune can be bounded preset outputs rather than continuous regressions; that keeps errors easier to constrain.

### What A Short-Window Classifier Does Not Deliver

A finite 20-50 ms window limits frequency discrimination and discards much of the longer musical context. Using the basic 1/T relation gives an approximate **20-50 Hz observation scale**; windowing and estimator choice matter, so this is not a hard pitch-resolution specification. Robust chord/root recognition, especially with distortion or reverb contamination, is a materially harder task than classifying brightness.

More importantly, this proposed architecture does **not provide continuous pitch trajectories, bend/vibrato tracking, or sample-accurate modulation**. Its outputs arrive as asynchronous control decisions. Any pitch processing, continuous modulation, interpolation, and smoothing remain in the existing CPU/DSP audio path. The exclusion is architectural, not a claim that no neural network could ever estimate pitch.

**Decision:** train and validate the modest acoustic task first. Add harmonic interpretation only when the product supplies enough context and the listening results justify it.


## 5. Synthesis: Choose The Smallest System That Improves Listening

The strongest evidence for using Neutron is the measured audio workload acceleration. The strongest caution is AN14241's substantial tensor arena, together with the absence of demonstrated accuracy and worst-case interrupt timing for this guitar application. Those facts favor a controlled experiment, not an unconditional feature commitment. [2][12]

| Approach | Mechanism and scope | Memory / scheduling trade-off | Evidence and time horizon | Recommendation |
|---|---|---|---|---|
| CPU spectral rules | Map a few interpretable features to fixed presets | Avoids a neural arena; feature extraction still costs CPU time | No guitar comparison is supplied by the cited NXP benchmarks; decisions can be per attack | Establish this baseline first |
| Small INT8 CNN plus policy | Learn texture distinctions; constrain musical choices outside the model | Adds model workspace, conversion, and inference scheduling | NXP audio benchmarks support feasibility, not guitar quality; one result per event | Preferred NPU prototype |
| Direct six-tension plus bloom/detune network | Learn the complete control preference mapping | Requires richer labels and more extensive musical validation | No cited result demonstrates this task | Consider only after simpler acoustic outputs prove useful |
| LSTM/SVDF or multiplicative-gated model | Add sequence/state processing | Important operators fall outside the explicit N1-16 accelerated baseline | Unsupported in the cited device table; CPU execution may remain possible | Avoid as the initial topology. [5] |

The non-obvious distinction is that **neural-network acceleration and product value are different comparisons**. A 15.76x keyword-model speedup over CPU inference does not show that a CNN beats a centroid/density rule in this instrument effect. Conversely, a rule that works for one clean pickup may prove brittle across the player's real signal chain; that is where a trained classifier might earn its memory and integration cost. [2]

For a go/no-go prototype, require all of the following:

- **Conversion:** the heavy computation lands in the intended `NeutronGraph` partition; any CPU fallback is understood and budgeted. [4]
- **Memory:** measured arena, extra workspace, snapshots, and stack margins fit after the real reverb allocation, not in an otherwise empty demo.
- **Audio continuity:** worst-case SAI service time and underrun behavior remain acceptable while inference and the full effect run together.
- **Musical benefit:** held-out recordings and blind listening comparisons beat the CPU rule across the intended guitars, pickups, dynamics, noise, and effects conditions.
- **Failure behavior:** ambiguous or late classifications retain a safe setting; parameters remain bounded and transitions remain smooth.

**Final recommendation: viable only under these constraints.** Use an event-triggered, compatible INT8 CNN to suggest a discrete control state, with the CPU retaining attack detection, harmonic policy, bloom/detune smoothing, and all audio DSP. If available SRAM or audio timing fails the prototype, or the classifier does not audibly outperform a simple rule, the NPU is not worth using for this feature. The primary unresolved implementation detail is the selected MCXN947 Neutron driver's exact asynchronous and interrupt behavior; treat that as an integration acceptance condition, not an assumed capability.

## References

1. *MCXN947 — MCUXpresso SDK Documentation*. https://mcuxpresso.nxp.com/mcuxsdk/latest/html/drivers/MCX/MCXN/MCXN947/index.html
2. *GitHub - nxp-appcodehub/dm-mcxn947-npu-vs-tensorflm-benchmark: This demo shows the performance benchmark between NPU and TensorFLM. Real time results are displayed on TFT LCD. On-chip NPU accelerates AI/ML algorithms and improves the performance. · GitHub*. https://github.com/nxp-appcodehub/dm-mcxn947-npu-vs-tensorflm-benchmark
3. *Highly integrated low-power dual-core Arm Cortex M33 MCU with on-chip accelerators*. https://www.nxp.com/docs/en/data-sheet/MCXNP184M150F70.pdf
4. *Neutron Converter - eIQ® Learning Hub*. https://eiq.nxp.com/learning-hub/tools/neutronSdk/softwareTools/NeutronConverter.html
5. [
	How to get started with NPU and ML in MCX Microcontrollers - NXP Community
](https://community.nxp.com/t5/MCX-Microcontrollers-Knowledge/How-to-get-started-with-NPU-and-ML-in-MCX-Microcontrollers/ta-p/1788440)
6. [
	How to Update eIQ Projects with the Latest eIQ Neutron SDK Libraries - NXP Community
](https://community.nxp.com/t5/i-MX-RT-Crossover-MCUs-Knowledge/How-to-Update-eIQ-Projects-with-the-Latest-eIQ-Neutron-SDK/ta-p/2270833)
7. [
	Re: MCXN947 eIQ Neutron NPU supported operators - NXP Community
](https://community.nxp.com/t5/MCX-Microcontrollers/MCXN947-eIQ-Neutron-NPU-supported-operators/m-p/2166153)
8. *TensorFlow Lite for Microcontrollers on NXP Neutron — Zephyr Project Documentation*. https://docs.zephyrproject.org/latest/samples/boards/nxp/tflm_neutron/README.html
9. *tflm_kws — MCUXpresso SDK Documentation*. https://mcuxpresso.nxp.com/mcuxsdk/latest/html/examples/eiq_examples/tflm_kws/readme.html
10. *eIQ Neutron - eIQ® Learning Hub*. https://eiq.nxp.com/learning-hub/tools/neutronSdk/softwareStack/inferenceEnginesFlows/eiqNeutron_inference.html
11. *Supported Operator List — MCUXpresso SDK Documentation*. https://docs.mcuxpresso.nxp.com/mcuxsdk/latest/middleware/eiq/tensorflow-lite/docs/topics/supported_operators.html
12. *How to Integrate Customer ML Model to NPU on MCX      N94x*. https://www.zlgmcu.com/data/upload/file/Utilitymcu/AN14241-%E5%A6%82%E4%BD%95%E5%9C%A8MCX%20N94x%E4%B8%8A%E5%B0%86%E5%AE%A2%E6%88%B7ML%E6%A8%A1%E5%9E%8B%E4%B8%8ENPU%E6%97%A0%E7%BC%9D%E9%9B%86%E6%88%90.pdf
