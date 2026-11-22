---
id: TASK-2
title: 24-bit 化後の DSP 負荷を実測し記録を更新する
status: To Do
assignee: []
created_date: '2026-11-22T15:46:00Z'
labels: []
dependencies: []
references:
  - https://github.com/divergen371/frdm_mcxn947_ambient_fx/issues/2
documentation:
  - docs/dsp-load-measurement.md
priority: high
type: docs
ordinal: 2000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
docs/dsp-load-measurement.md の占有率は 16 bit・32 タップ時点の値。現行は 24 bit Q23、64 タップ FIR、4 ボイスシマー、12 プリセット。
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Release ビルドを実機へ書き込み、UART の [DSP] 行で全プリセットの平均サイクルと占有率を取る
- [ ] #2 原音は Shoegaze Glide Chord とし、P9 と P10 の聴感メモを残す
- [ ] #3 表を docs/dsp-load-measurement.md に更新し、計測条件が 24 bit であることを明記する
<!-- AC:END -->
