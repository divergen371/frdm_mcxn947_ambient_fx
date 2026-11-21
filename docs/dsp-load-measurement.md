# DSP 負荷実測レポート(FRDM-MCXN947 / 48 kHz)

本書は、`src/main.c` の計測機構で取得した DSP 実行コストの実測値を記録する。以降の最適化判断は推測ではなくここの数字を基準にする。

## 1. 計測方法

- 計測点: `sai_fill_fifo()` のうち L チャネル分岐。`audio_synth_next_sample()` と `ambient_reverb_process_stereo()` を挟んで DWT サイクルカウンタ (`DWT->CYCCNT`) の差分を取る。
- 1 フレーム = L/R ペア 1 組。48 kHz ステレオなので毎秒 48,000 フレームになる。
- 集計は `volatile` な 32 bit 変数への加算のみとし、UART 出力はメインループ側で 5 秒ごとに行う。ISR 内では `PRINTF` を呼ばない。
- 予算 (1 サンプル周期) は `SystemCoreClock / AUDIO_SAMPLE_RATE_HZ = 150,000,000 / 48,000 = 3,125` サイクル。占有率はこの比。
- 計測条件: `release` プリセット (`-O3 -flto=auto`)、原音は Shoegaze Glide Chord。
- 各 5 秒区間のフレーム数は約 240,000 (= 48,000 × 5) で、区間平均は十分に安定していた。

`DWT->CYCCNT` は 32 bit で 150 MHz では約 28.6 秒で一周するため、区間長を 5 秒に収めている。

## 2. 計測オーバーヘッド

計測コード自体は 1 フレームあたり概算 12〜15 命令 (`MSDK_GetCpuCycleCount()` 2 回、加算、最大値比較、カウンタ加算)。

`Mute / Off` は DSP 本体が即 return するため、**138 サイクルが計測と原音生成と関数呼び出しの固定コスト**とみなせる。下表の値はこの固定コストを含む。

## 3. 実測結果(全プリセット)

| # | プリセット | 平均サイクル/フレーム | 占有率 | 最大 |
|---|---|---|---|---|
| 1 | SPX90 Reverse Gate | 805 | 25.76 % | 821 |
| 2 | SPX90 Early Reflections | 802 | 25.66 % | 818 |
| 3 | Dimension + Harmonic Pan (Sine x Sine) | 787 | 25.18 % | 1044 |
| 4 | Dimension + Infinite Cross-Ramp | 768 | 24.57 % | 809 |
| 5 | Dimension + Stardust Pan | 781 | 24.99 % | 821 |
| 6 | Dimension + Psyche Chop | 793 | 25.37 % | 839 |
| 7 | 4x4 Hadamard FDN + Shimmer | 555 | 17.76 % | 802 |
| 8 | Shoegaze Cathedral (All-in) | 1219 | 39.00 % | 1261 |
| 9 | Bypass (Dry) | 144 | 4.60 % | 1249 |
| 10 | Mute / Off | 138 | 4.41 % | 150 |

「最大」列はプリセット切替直後の過渡を含むため、定常状態の値ではない。平均列が設計判断の基準になる。

## 4. 読み取れること

- 最も重い **Shoegaze Cathedral でも 39.0 %**。48 kHz フルレート 4 段構成としては余裕が残っている。締切超過の兆候 (最大値の突出) も定常状態では見られない。
- Reverse Gate / Early Reflections は 32 タップ FIR を毎サンプル回しているが、25 % 台に収まっている。FDN とシマーを含む Shimmer Hall の方が軽い (17.8 %) のは、タップ FIR の 32 回の積和が支配的だからである。
- Dimension 系 4 種は 24.6〜25.4 % でほぼ横並び。パン波形の違い (サイン / ランプ / ランダム / 矩形) は負荷にほとんど影響しない。
- 固定コスト 138 サイクル (4.4 %) の内訳は、原音生成、関数呼び出し、計測自体である。DSP 本体の実コストは「平均 − 138」で概算できる。

## 5. この数字が意味する制約

1 フレームの予算 3,125 サイクルに対し、最悪ケースで約 1,080 サイクル余っている。したがって、

- **新しいエフェクト段を 1 つ追加する余地はある**(Shoegaze Cathedral 相当の重さの段をもう 1 段足すと 70 % 前後になる)。
- ただし FDN はサンプル単位のフィードバック閉路なので、PowerQuad のブロック処理 (FIR/FFT) に置き換えられない。負荷の大部分はここではなくタップ FIR 側にある。
- したがって最適化の優先順位は、**タップ FIR (Reverse Gate / Early Reflections / Cathedral) > Dimension 系 > FDN** となる。

## 6. 再計測の手順

`src/main.c` はビルド後そのまま計測値を 5 秒ごとに UART へ出す。追加のビルドフラグは不要。

```
./build.sh                       # または cmake --build --preset release
LinkServer flash MCXN947 load release/frdm_mcxn947_ambient_fx_cm33_core0.elf
```

UART (115200 bps) を開くと次の形式で出力される。

```
[DSP] avg 1219 cyc/frame (39.00% of 3125) max 1261 over 240000 frames | 8: Shoegaze Cathedral (All-in 48kHz)
```

プリセットを切り替えると集計は自動でリセットされ、切替後の定常状態だけが次区間に反映される。
