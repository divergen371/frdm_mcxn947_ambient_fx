# FRDM-MCXN947 I2C OLED表示およびUI設計書

## 1. 目的と方針

本書は、FRDM-MCXN947上の48 kHzフルレートDSPエフェクタへ128x64モノクロI2C OLEDディスプレイを追加し、プリセット、テスト音源、およびロータリーエンコーダで編集する音色パラメータをPCなしで視認できるようにするためのハードウェア設計およびソフトウェア設計を定める。

本プロジェクトは10種のプリセット、9項目の編集パラメータ、および高域・低域それぞれ6種の独立パンニング波形を持つ。オンボードRGB LEDとシリアルログのみでは編集対象と現在値を把握しづらいため、OLED表示をロータリーエンコーダ設計書 `docs/encoder-design.md` と一体のUI基盤として位置付ける。

設計の基本方針は以下の3点である。

- 48 kHzリアルタイム音声処理の割り込み応答とFIFO充填へ一切影響を与えない。
- エンコーダ配線とピンが衝突しない既存の温存バス `LPI2C2` を使用する。
- OLED未接続または通信失敗時にもオーディオ処理と既存スイッチ操作を継続できるフェイルセーフ構成とする。

---

## 2. ハードウェア設計と配線

### 2.1 対象モジュール仕様

対象は電子工作で広く流通している128x64ドット・モノクロI2C OLEDモジュールとする。

- コントローラIC: SSD1306またはSH1106とする。0.96インチ品は主にSSD1306、1.3インチ品は主にSH1106を搭載する。両者はI2Cコマンド体系がほぼ共通であり、列アドレスオフセットと初期化シーケンスのみコンパイル時定数で切り替えられるようにする。
- 通信方式: I2C 2線式とし、7-bitスレーブアドレスは標準の `0x3C` を既定とする。ジャンパ切替品の `0x3D` にも対応する。
- 電源電圧: 3.3 V動作とする。5 V対応レギュレータを搭載したモジュールであっても、I2C信号線の電圧をMCXN947の3.3 V GPIO定格へ確実に揃えるため、電源は必ず基板の3.3 Vピンから供給する。

### 2.2 FRDM-MCXN947配線表

ピン割り当てはMCUXpresso SDKのFRDM-MCXN947 LPI2C2サンプル実装と照合して確定した。`docs/encoder-design.md` で定義したエンコーダ用ピンおよび `SAI1` オーディオピンとは1本も重複しない。

| OLED端子 | FRDM-MCXN947ヘッダ | MCUピン | MUX設定 | 備考 |
|---|---|---|---|---|
| `SDA` | `J2[18]` | `P4_0` | `ALT2` `FC2_P0` | `LPI2C2_SDA` |
| `SCL` | `J2[20]` | `P4_1` | `ALT2` `FC2_P1` | `LPI2C2_SCL` |
| `GND` | `J2[14]` | `GND` | なし | `ENC0` のGNDと共通レールで分岐可能 |
| `VCC` | `J3[8]` | `P3V3` | なし | 5 Vピン `J3[10]` へは接続しない |

### 2.3 共用I2CバスとSDK設定

`LPI2C2` はFRDM-MCXN947上の加速度センサやコーデックI2C配線と同一バスである。オンボードデバイスのアドレス帯は `0x18` から `0x1A` 付近であり、OLEDの `0x3C` および `0x3D` とは衝突しない。

ファームウェア側で必要なSDKおよびクロック設定は以下の通りである。

- `prj.conf` への追加: `CONFIG_MCUX_COMPONENT_driver.lpflexcomm_lpi2c=y`
- ポートクロック有効化: `CLOCK_EnableClock(kCLOCK_Port4)`
- ペリフェラルクロック設定: `CLOCK_AttachClk(kFRO12M_to_FLEXCOMM2)` および `CLOCK_SetClkDiv(kCLOCK_DivFlexcom2Clk, 1U)` とし、12 MHzソースクロックから400 kHz Fast Modeを生成する
- ポート設定: `PORT4` の0番ピンおよび1番ピンを `kPORT_MuxAlt2`、内部プルアップ有効、デジタル入力有効に設定する

---

## 3. リアルタイムオーディオとの両立設計

### 3.1 割り込み優先度と実行コンテキスト

OLEDのI2C転送はメインループの非割り込みコンテキストでのみ実行する。

1. `SAI1_IRQn`: NVIC優先度 `2` で48 kHz音声生成とFIFO充填を実行する。
2. `SysTick_IRQn`: NVIC優先度 `6` で1 ms周期のエンコーダ読み取りとスイッチ判定を実行する。
3. メインループ: エンコーダ差分反映、パラメータ更新、UARTログ出力、およびOLED差分転送を実行する。

この階層により、メインループがI2Cの1バイト送信完了を待っている最中でも `SAI1_IRQn` と `SysTick_IRQn` が即座にプリエンプトする。したがってOLED描画が音声アンダーランやエンコーダのステップ取りこぼしを引き起こすことはない。

### 3.2 フレームバッファと差分転送

128x64モノクロ画面のフレームバッファは1,024バイトである。現在のSRAM使用量は約60.7 KBであり、1 KBのフレームバッファとフォントROMを追加してもSRAM使用率は20%未満に収まる。

I2C通信量を抑えてエンコーダ操作への追従性を高めるため、画面を縦8ピクセル単位の8ページへ分割し、ビットマスク `dirty_pages` で変更があったページのみを転送する。

- 400 kHz I2Cでの1ページ転送時間: 制御バイトと128バイトのデータ送信で約3.0 msである。
- 全8ページ一括更新時: 約24 msである。
- リフレッシュ間隔の上限: 最小33 ms間隔、すなわち最大30 Hzへ制限し、ノブを連続回転させてもI2Cバスを占有し続けないようにする。

### 3.3 フェイルセーフ動作

初期化時に `0x3C` へプローブ送信を行い、ACKが返らない場合は `g_oled_ready = false` として以降のI2C転送をすべてスキップする。通信途中でバスエラーやNACKが発生した場合もタイムアウト上限を設けて早期復帰し、メインループの停止やオーディオ無音化を防ぐ。

---

## 4. 画面レイアウトとUI設計

### 4.1 3ゾーン画面構成

市販の0.96インチ2色OLEDでは上部16ピクセルが黄色、下部48ピクセルが青色に分かれている。2色OLEDと単色OLEDの双方で見やすくするため、画面を以下の3ゾーンに分割する。

- Zone A: y=0から15、Page 0から1。現在のプリセット番号と名称、右端にテスト音源の略号を表示する。
- Zone B: y=16から47、Page 2から5。現在エンコーダで編集中のパラメータ名、大きな数値、意味付けされた補助表記、および横バーグラフまたは波形アイコンを表示する。
- Zone C: y=48から63、Page 6から7。主要4パラメータのミニバーを常駐表示する。プリセットリセットなどの操作直後は1.2秒間だけトースト通知へ切り替える。

### 4.2 通常パラメータ編集時のレイアウト

```text
+--------------------------------+  y=0  Zone A (16px: Yellow)
| P8 Shoegaze Cath.       [GLID] |
+--------------------------------+  y=16 Zone B (32px: Blue)
| > MIX               175        |
|   Dry 31% / Wet 69%            |
|   [################......]     |
+--------------------------------+  y=48 Zone C (16px: Blue)
| MIX:### DEC:### SHM:##  PAN:## |
+--------------------------------+  y=63
```

テスト音源の略号は `GLID`、`PLUK`、`SAW` の3種とする。補助表記はパラメータ種別に応じて切り替える。

- `Mix`: `Dry XX% / Wet YY%` を表示する。
- `Pan Rate`: 計算上の実周波数 `0.65 Hz` などを表示する。
- `Pan Depth` / `Decay` / `Shimmer` / `Dimension` / `Tap Level`: 0%から100%の相対値を表示する。

### 4.3 パンニング波形選択時のレイアウト

`Pan Hi Wave` または `Pan Lo Wave` を選択している間は、Zone Bに高域と低域の波形名および24x12ピクセルのドット波形アイコンを上下に並べて表示する。

```text
+--------------------------------+  y=0  Zone A
| P4 Cross-Ramp           [GLID] |
+--------------------------------+  y=16 Zone B
| > HI: RampUp       / / /       |
|   LO: RampDown     \ \ \       |
+--------------------------------+  y=48 Zone C
| RATE:0.67Hz  DEPTH:80%  DIM:ON |
+--------------------------------+  y=63
```

6種の波形それぞれに視認しやすい1周期分の波形パターンを用意する。

- `Sine`: 正弦曲線を描く。
- `Triangle`: 直線の山谷を描く。
- `RampUp`: 右肩上がりののこぎり波を描く。
- `RampDown`: 右肩下がりの逆のこぎり波を描く。
- `SoftSquare`: 角を丸めた矩形波を描く。
- `SmoothRand`: 高低の不規則な階段状曲線を描く。

### 4.4 エンコーダおよびスイッチ操作との連動

- `SW2` 押下: Zone Aのプリセット名とZone Cのミニバーを即時更新し、全8ページをdirtyにする。
- `SW3` 押下: Zone A右端の音源略号のみを更新し、Page 0から1のみをdirtyにする。
- `ENC0` 回転: Zone Bの数値・バーグラフ・波形アイコンと、対応するZone Cのミニバーを更新する。
- `ENC0` 短押し: 編集対象カーソル `>` を次項目へ進め、Zone Bを再描画する。
- `ENC0` 長押し: プリセット初期値へリセットし、Zone Cへ `RESET TO DEFAULT` を1.2秒間表示する。

---

## 5. ソフトウェアモジュール設計

### 5.1 ファイル構成

描画ドライバと画面レイアウト責務を分離し、将来SPI OLEDや大型液晶へ乗り換える際の影響範囲を最小化する。

- `src/oled_i2c.h` / `src/oled_i2c.c`: `LPI2C2` 初期化、SSD1306およびSH1106コマンド送信、1,024バイトフレームバッファ、dirtyページ差分転送、5x7フォントおよび基本図形描画を担う。
- `src/ui_display.h` / `src/ui_display.c`: プリセット、音源、編集対象、`ambient_params_t` の状態を受け取り、Zone A・B・Cのレイアウトと波形アイコンをフレームバッファへ描画する。
- `src/main.c`: 起動時に `oled_i2c_init` と `ui_display_init` を呼び出し、メインループで操作イベント発生時に `ui_display_mark_dirty` を立てて `ui_display_service` を実行する。

### 5.2 ドライバAPI設計

```c
#ifndef OLED_I2C_H
#define OLED_I2C_H

#include <stdbool.h>
#include <stdint.h>

#define OLED_WIDTH  128U
#define OLED_HEIGHT 64U

#ifndef OLED_I2C_ADDR
#define OLED_I2C_ADDR 0x3CU
#endif

#ifndef OLED_CONTROLLER_SH1106
#define OLED_CONTROLLER_SH1106 0
#endif

bool oled_i2c_init(void);
bool oled_i2c_is_ready(void);
void oled_clear(void);
void oled_set_pixel(uint8_t x, uint8_t y, bool on);
void oled_draw_hline(uint8_t x, uint8_t y, uint8_t w, bool on);
void oled_fill_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool on);
void oled_draw_char(uint8_t x, uint8_t y, char ch, uint8_t scale, bool invert);
void oled_draw_text(uint8_t x, uint8_t y, const char *str, uint8_t scale, bool invert);
void oled_flush_dirty(void);

#endif /* OLED_I2C_H */
```

`oled_flush_dirty` は1回の呼び出しで変更されたページのみを転送し、転送完了後に該当ページのdirtyビットをクリアする。`OLED_CONTROLLER_SH1106` を `1` に設定した場合はページアドレッシング時の開始列オフセットへ `+2` を加算する。

### 5.3 UIレイヤAPI設計

```c
typedef enum
{
    UI_PARAM_MIX = 0,
    UI_PARAM_PAN_DEPTH,
    UI_PARAM_PAN_RATE,
    UI_PARAM_PAN_HI_WAVE,
    UI_PARAM_PAN_LO_WAVE,
    UI_PARAM_DECAY,
    UI_PARAM_SHIMMER,
    UI_PARAM_CHORUS,
    UI_PARAM_TAP_LEVEL,
    UI_PARAM_COUNT
} ui_param_id_t;

void ui_display_init(void);
void ui_display_set_state(ambient_preset_t preset,
                          audio_synth_src_t src,
                          const ambient_params_t *params,
                          ui_param_id_t selected_param);
void ui_display_show_toast(const char *msg, uint32_t duration_ms);
void ui_display_service(uint32_t now_ms);
```

---

## 6. 段階的実装手順と実機検証チェックリスト

### 6.1 実装フェーズ

1. Phase 1: `prj.conf` へ `CONFIG_MCUX_COMPONENT_driver.lpflexcomm_lpi2c=y` を追加し、`src/oled_i2c.h` / `src/oled_i2c.c` を実装して単体表示テストを行う。
2. Phase 2: `src/ui_display.h` / `src/ui_display.c` を実装し、エンコーダ未接続の状態でも既存の `SW2` と `SW3` でプリセット・音源・パラメータ初期値がOLEDへ正しく反映されることを確認する。
3. Phase 3: `docs/encoder-design.md` の `src/rotary_encoder.h` / `src/rotary_encoder.c` およびDSPパラメータスムージングを統合し、ノブ操作とOLED表示を連動させる。

### 6.2 実機検証チェックリスト

- 電源電圧確認: OLEDモジュールの `VCC` が3.3 Vであり、5 Vラインへ誤接続されていないこと。
- バス認識確認: 起動時にアドレス `0x3C` からACKが返り、文字化けや画面左右の2ピクセルずれが発生していないこと。左右端にごみ表示が出る場合は `OLED_CONTROLLER_SH1106` を切り替えて確認する。
- 表示連動確認: `SW2` と `SW3` の切替、およびエンコーダの回転・短押し・長押しに対して画面表示が遅延なく追従し、波形アイコンが6種すべて正しく描画されること。
- オーディオ非干渉確認: `Shoegaze Cathedral` 再生中にエンコーダを高速回転させてOLEDを連続更新しても、聴感上のノイズや音切れが発生せず `SAI1` の `kSAI_FIFOErrorFlag` が立たないこと。
- フェイルセーフ確認: OLEDの配線を外した状態でリセット起動してもハングせず、音声再生と `SW2` / `SW3` / UARTログが正常に動作すること。
