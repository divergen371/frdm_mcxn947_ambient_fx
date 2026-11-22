---
id: TASK-3
title: ロータリーエンコーダで DSP パラメータを相対操作する
status: To Do
assignee: []
created_date: '2026-11-22T15:46:00Z'
labels: []
dependencies: []
references:
  - https://github.com/divergen371/frdm_mcxn947_ambient_fx/issues/3
documentation:
  - docs/encoder-design.md
priority: medium
type: feature
ordinal: 3000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
プリセットと音源の切替は SW2 / SW3 のみ。設計は docs/encoder-design.md。EC11 または KY-040 を J2 に接続し、電源は 3.3 V のみ。QDC と PINT は使わない。
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 SysTick 1 ms の Gray コード読み取りで、最初は 1 個、ソース構造は 4 個まで増やせる
- [ ] #2 プッシュで対象パラメータを切り替え、回転は現在値からの相対増減にする
- [ ] #3 decay、shimmer、chorus、mix、pan_depth、pan_rate、tension を操作でき、連続値は 1 次スムージングする
<!-- AC:END -->
