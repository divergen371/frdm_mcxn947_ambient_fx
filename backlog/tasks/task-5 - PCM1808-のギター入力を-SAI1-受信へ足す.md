---
id: TASK-5
title: PCM1808 のギター入力を SAI1 受信へ足す
status: To Do
assignee: []
created_date: '2026-11-22T15:46:00Z'
updated_date: '2026-11-22T15:46:00Z'
labels: []
dependencies:
  - TASK-2
references:
  - https://github.com/divergen371/frdm_mcxn947_ambient_fx/issues/5
priority: low
type: feature
ordinal: 5000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
現行の原音は内蔵テスト音源のみ。DSP はすでに 24 bit Q23 で、PCM1808 の 24 bit I2S を受ける前提になっている。
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 1 MΩ ギターバッファと PCM1808 を SAI1_RX に接続するピンとクロックを確定する
- [ ] #2 内蔵音源と外部入力を切り替えられる
- [ ] #3 入力ゲイン、DC ブロッカ、ノイズフロアを実ギターで確認する
<!-- AC:END -->
