# FRDM-MCXN947 + PCM5102A 48 kHz フルレート アンビエント／シューゲイザー DSP

NXP **FRDM-MCXN947**（Cortex-M33 @ 150 MHz / 512 KB SRAM / SAI1 + Audio PLL1）上で、dsPIC33CK で検証した **SPX90 風 Reverse Gate / Early Reflections ＋ 酩酊 3 相 6 Hz コーラス ＋ 4×4 Hadamard FDN ＋ シマー（Fuzz 抜き）** を **全段 48,000 Hz フルレート・間引きゼロ** で実行する実験プロジェクトです。

---

## 1. 配線表（FRDM-MCXN947 UM12018 Table 17 / 19 準拠・基板無改造）

| FRDM-MCXN947 ヘッダ | MCU ピン / ALT | 機能 | PCM5102A |
|---|---|---|---|
| **`J1[1]`** | `P3_16` / `ALT10` (`SAI1_TX_BCLK`) | I²S ビットクロック (3.072 MHz, 64fs) | **`BCK`** |
| **`J1[11]`** | `P3_17` / `ALT10` (`SAI1_TX_FS`) | I²S ワードクロック (48.000 kHz) | **`LCK`** |
| **`J1[5]`** | `P3_20` / `ALT10` (`SAI1_TXD0`) | I²S データ (SDATA) | **`DIN`** |
| **`J3[12]`** または **`J3[14]`** | `GND` | 共通 GND | **`GND`** |
| **`J3[10]`** (5V) または **`J3[8]`** (3.3V) | `P5V0` / `P3V3` | モジュール電源 | **`VIN`** |

- **電源の注意**: PCM5102A ブレークアウト基板上に 3.3V LDO が載っていれば `J3[10]` (5V)、無ければ `J3[8]` (3.3V) へ接続してください。
- **PCM5102A 設定**: `SCK` = GND（内部 PLL 動作）、`FMT` = GND（標準 I²S）、`XSMT` = High（ミュート解除）。
- **ライン出力**: 3.5mm ジャックはアンプ内蔵スピーカー、ヘッドホンアンプ、またはライン入力へ接続してください。

---

## 2. クロック設計（理論誤差 0 ppm）

- 外部水晶 `24.000 MHz` → Audio PLL1（`NDIV=25, PDIV=2, MDIV=512`）→ **`122.880 MHz`**
- `PLL1_CLK0`（÷10）→ **`12.288 MHz`** → `SAI1`
- `SAI1` I²S Master（24-bit ステレオ / 32-bit スロット = 64fs フレーム、PCM5102A / PCM1808 互換）:
  - **`BCLK = 3,072,000.00 Hz`**
  - **`LRCK = 48,000.00 Hz`**（水晶精度、理論誤差 0.0000%）

---

## 3. 操作方法（オンボード SW2 / SW3 / RGB LED / UART）

### `SW2`（`P0_23`）: プリセット切替
1. **Reverse Gate**（赤 LED）: SPX90 風 48 kHz 前段 4 段変調拡散 ＋ 64 タップ逆エンベロープ FIR（255 ms）
2. **Early Reflections**（緑 LED）: 48 kHz 前段 2 段拡散 ＋ 64 タップ立体初期反射 FIR
3. **Dimension + Harmonic Pan (`Sine × Sine`)**（青 LED）: 揺れない極ワイド Dimension Chorus ＋ 滑らかサイン波ハーモニック・パンニング
4. **Dimension + Infinite Cross-Ramp (`RampUp × RampDown`)**（黄 LED）: 高域と低域が逆方向へ無限にすれ違い続ける酩酊旋回
5. **Dimension + Stardust Pan (`SmoothRand × Sine`)**（シアン LED）: 揺れる低域の上で高域だけが星屑のようにランダム浮遊
6. **Dimension + Psyche Chop (`SoftSquare × Sine`)**（マゼンタ LED）: 低域のうねりの上で高域が左右に跳ねるサイケ・ハーモニックパン
7. **Shimmer Hall**（シアン LED）: 48 kHz 4×4 Hadamard FDN ＋ 42.7 ms 窓 +1 オクターブ・シマー
8. **Shoegaze Cathedral**（マゼンタ LED）: Reverse Gate ＋ Dimension ＋ FDN ＋ シマー ＋ 緩やかなハーモニック・パンニング全部盛り
9. **Bypass**（白 LED）: 原音比較用
10. **Off**（消灯）: 無音（I²S クロック維持）

### `SW3`（`P0_6`）: テスト原音切替
1. **Shoegaze Glide Chord**（Fmaj7 → G6 → Asus4 → G9sus4 ＋ アーム揺らし）← 起動時
2. **Bell Pluck Arpeggio**（C5 → E5 → G5 → B5 → 休符）
3. **Sawtooth 220 Hz**

※ MCU-Link USB 仮想シリアル（115200 bps）を開くと、現在のプリセット名・原音名が表示されます。

---

## 4. ビルド・書き込み手順

### ビルド

```zsh
cd ~/frdm_mcxn947_ambient_fx
./build.sh
```

生成物：
- `build/frdm_mcxn947_ambient_fx_cm33_core0.elf`
- `build/frdm_mcxn947_ambient_fx_cm33_core0.bin`
- `build/frdm_mcxn947_ambient_fx_cm33_core0.hex`

### 書き込み

FRDM-MCXN947 の MCU-Link USB ポート（`J17`）を Mac へ接続し、LinkServer / MCUXpresso for VS Code / `west flash` 等で `build/frdm_mcxn947_ambient_fx_cm33_core0.elf`（または `.bin` / `.hex`）を書き込んでください。
