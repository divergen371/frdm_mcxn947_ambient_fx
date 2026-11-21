# 対話コンテキスト

- 作業名: FRDM-MCXN947+PCM5102A 48 kHzフルレート アンビエント・シューゲイザーDSP移植プロジェクト
- ブランチ: 未取得
- 参照コミット: 未取得
- 関連PR: 未作成
- Updated at: 2026-11-21T16:33:00+09:00
- 作成者・更新者: atsushi / Cursor Agent

## 目的

1. `dsPIC33CK64MC105`で開発したFuzz抜きのシューゲイザー・アンビエント特化エフェクタを、NXP `FRDM-MCXN947`へ移植し、512 KB SRAMと150 MHz Cortex-M33の余裕を活かした48 kHzフルレート・間引きゼロ構成で実機音出し実験を行う。
2. 搭載エフェクトはYamaha SPX90着想の32タップFIRによるReverse GateおよびEarly Reflections、軽い2段オールパス拡散、0度・120度・240度の酩酊3相6 Hzコーラス、4x4 Hadamard FDN、42.7 ms窓の+1オクターブシマーとする。
3. 実機で`PCM5102A`から音出し確認を行い、必要に応じてチューニングや将来のADCノブ・I2S入力・SPX90実機IR移植へ進める基盤とする。

## 採用した設計と理由

- ビルド基盤として導入済みのMCUXpresso SDK `/Users/atsushi/MCXN947/frdm_mcxn947/mcuxsdk`をアウトオブツリー参照する`west build`構成を採用し、`build.sh`へ集約した。
  - ツールチェーンは`/Users/atsushi/.mcuxpressotools/.mcux-venv-3.12/bin`の`west`と`/Users/atsushi/.mcuxpressotools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi`の`arm-none-eabi-gcc 14.2.1`を使う。
  - `CMakeLists.txt`で`-O2`を指定し、リアルタイムDSP ISRの締め切り超過を防ぐ。
- オーディオクロックは外部24 MHz水晶からAudio PLL1で`122.880 MHz`を生成し、`kCLOCK_DivPLL1Clk0=10`で`12.288 MHz`を`SAI1`へ供給する構成を採用した。
  - `FRO_HF`48 MHzでは48 kHz I2Sを整数分周できないため、SDKの`pdm_sai_interrupt`と同じPLL1設定`NDIV=25`、`PDIV=2`、`MDIV=512`を使い、理論誤差0 ppmの`BCLK=1.536 MHz`、`LRCK=48.000 kHz`を実現した。
- `SAI1`のピン配置はUM12018 Table 17の既定接続に従い、基板無改造で配線できる`J1`ヘッダの3ピンを採用した。
  - `J1[1]` `P3_16` `ALT10`を`SAI1_TX_BCLK`、`J1[11]` `P3_17` `ALT10`を`SAI1_TX_FS`、`J1[5]` `P3_20` `ALT10`を`SAI1_TXD0`へ割り当てた。
  - 電源とGNDはUM12018 Table 19に従い、`J3[12]`または`J3[14]`をGND、`J3[10]`を5V、`J3[8]`を3.3Vとした。
- `dsPIC33CK`版でRAM 8 KB制約のために導入していた16 kHzと8 kHzの間引きを撤廃し、`src/ambient_reverb.c`で全段48 kHzフルレート・独立バッファ構成へ刷新した。
  - 32タップFIRに16,384サンプル、4x4 Hadamard FDNに素数長1,487/1,753/2,053/2,381サンプル、3相6 Hzコーラスに1,024サンプル、シマーに2,048サンプルの42.7 ms窓を独立確保し、合計RAMを約58 KBへ収めた。
  - FDNの極性分散入力と差動出力、シマー帰還経路のDCブロッカと2段LPFを継承し、長時間稼働時のDC張り付きと高域折り返し雑音を防いだ。
- オンボードUIは`SW2` `P0_23`でプリセット7種を切替、`SW3` `P0_6`でテスト原音3種を切替、RGB LEDでプリセット色を表示、MCU-Link UART 115200 bpsへ切替ログを出力する構成とした。
  - 起動時の既定原音はケヴィン・シールズ風のグライドコード`SYNTH_SRC_SHOEGAZE_GLIDE`、既定プリセットは`AMBIENT_PRESET_REVERSE_GATE`とした。

## 却下した代替案と理由

- `dsPIC33CK`版の16 kHzと8 kHz間引きロジックをそのまま移植する案は却下した。
  - `FRDM-MCXN947`は512 KB SRAMを持ち、間引きによる高域劣化やアンチイメージングフィルタの制約を負う理由がないためである。
- SDK既定の`project_board_port_path`に依存する`pin_mux.c`と`hardware_init.c`を使う案は却下した。
  - アウトオブツリー配置ではSDK内の未存在パスを参照してCMake生成が失敗するため、`prj.conf`で`pinmux_customize_folder`と`hw_app_customize_folder`を有効化し、`src/board_audio.c`で完結させた。
- Fuzzエフェクトの組み込みはユーザー方針により除外した。

## 意図的に対応しない事項

- 外部音声入力`PCM1808`、可変抵抗ノブのADC読み取り、およびDMA転送化は初期実験スコープ外とし、まずは`SAI1`FIFO割り込みと内蔵テスト音源による実機音出しを優先した。
- 本ディレクトリはまだGitリポジトリ初期化していない。

## 発見された制約

- `FRDM-MCXN947`のMCU-Link書き込みツールは`west`環境や`openocd`はあるが、`LinkServer`と`pyocd`は`PATH`上に見つからなかった。書き込み手段はユーザー環境での実機接続時に確定する必要がある。
- `MCUXpresso SDK`のビルド設定で`-Werror`が既定有効のため、すべての新規・変更コードは警告ゼロを維持する必要がある。
- 出力バイナリ名はコアID接尾辞付きで`build/frdm_mcxn947_ambient_fx_cm33_core0.elf`、`.bin`、`.hex`になる。
- `PCM5102A` IC単体の定格は3.3Vであり、`J3[10]`の5Vを使う前に対象ブレークアウト基板上の3.3V LDO実装と導通を必ず確認する必要がある。

## 検証した事実と根拠

- 一次資料照合: MCUXpresso SDK内の`board.h`、`clock_config.c`、`pdm_sai_interrupt/cm33_core0/hardware_init.c`、および`UM12018` FRDM-MCXN947 Board User Manual Table 17とTable 19でSAI1ピン、ALT10、PLL1 122.88 MHz設定、SW2/SW3、RGB LED、J1/J3電源ピンを確認した。
- ビルド検証: `/Users/atsushi/frdm_mcxn947_ambient_fx/build.sh -p always`を実行し、`arm-none-eabi-gcc 14.2.1`、`-O2`、`-Werror`環境でエラー0・警告0でビルド成功した。
  - リンク結果: `m_text` 18,116 B 2.31%、`m_data` 58,968 B 18.46%。
  - 生成確認: `build/frdm_mcxn947_ambient_fx_cm33_core0.elf`、`.bin`、`.hex`。
- 移植元の実機実績: 同一アルゴリズムの原型は`/Users/atsushi/MPLABProjects/dsPIC`の`feat/ambient-reverb-pedal`ブランチコミット`7069efb`で`dsPIC33CK64MC105`実機音出しまで確認済みである。

## 未確認事項

- `FRDM-MCXN947`実機への書き込み手段と書き込み成功。
- `J1[1]`の`BCK` 1.536 MHz、`J1[11]`の`LCK` 48.000 kHz、`J1[5]`の`DIN`の実波形観測。
- `PCM5102A`接続時の実機音出し、左右チャンネル分離、および7プリセットと3原音の聴感確認。
- `SW2`、`SW3`、RGB LED、およびMCU-Link UART 115200 bpsログの実機動作。
- `SAI1`のFIFOウォーターマーク割り込み頻度と150 MHz Cortex-M33上の実CPU負荷、およびアンダーラン有無。

## 注意が必要な難所

- `prj.conf`から`CONFIG_MCUX_PRJSEG_module.board.pinmux_customize_folder=y`または`CONFIG_MCUX_PRJSEG_project.hw_app_customize_folder=y`を外すと、SDK側の未存在`cm33_core0/pin_mux.c`を参照してCMake生成が失敗する。
- `SAI1`のI2S Master設定で`mclkConfig.mclkSourceClkHz`と`SAI_TxSetBitClockRate`のソースクロックはいずれも`12288000U`に合わせる必要がある。`BOARD_InitHardware`で`CLOCK_SetClkDiv(kCLOCK_DivPLL1Clk0, 10U)`を変えた場合は両方追従させる。
- 移植元`dsPIC33CK`での実機知見として、FDNリングバッファの読み出しは`wr_idx + offset`の正方向で最古サンプル付近を読む必要がある。負方向へ引くと最新サンプルを読みリバーブが消える。
- 無音時や異音時は、電源とLDO、PCM5102Aの`XSMT`/`FMT`/`SCK`、`J1`ヘッダ配線、`BCK`/`LCK`/`DIN`波形、`SAI1`の`kSAI_FIFOErrorFlag`の順に切り分ける。

## 残作業

- `FRDM-MCXN947`と`PCM5102A`を`README.md`の配線表通りに接続し、ビルド済みバイナリを実機へ書き込んで音出しとUARTログを確認する。
- 48 kHzフルレート版の聴感を確認し、Reverse Gateタップ長、3相6 Hzコーラスの深さ、FDNダンピング周波数、シマー帰還量を実機で追い込む。
- 余裕のある約450 KBの空きSRAMと150 MHz CPUを活用し、必要に応じてeDMAピンポン転送化、48 kHzでのさらなるロングリバースや高次FDN拡張、FPU/DSP命令活用、またはYamaha SPX90実機IRの移植へ進める。
- 必要に応じて`/Users/atsushi/frdm_mcxn947_ambient_fx`をGitリポジトリとして初期化する。
