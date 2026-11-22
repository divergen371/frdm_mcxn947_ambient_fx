---
id: TASK-4
title: I2C OLED にプリセットとパラメータを表示する
status: To Do
assignee: []
created_date: '2026-11-22T15:46:00Z'
updated_date: '2026-11-22T15:46:00Z'
labels: []
dependencies:
  - TASK-3
references:
  - https://github.com/divergen371/frdm_mcxn947_ambient_fx/issues/4
documentation:
  - docs/oled-ui-design.md
priority: medium
type: feature
ordinal: 4000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
表示設計は docs/oled-ui-design.md。エンコーダ操作の対象が画面で分からないと相対操作が使いにくい。LPI2C2 を J2[18] / J2[20] に割り当て、電源は 3.3 V。
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 dirty ページだけの差分転送で、プリセット、操作中パラメータ、パン波形の 3 ゾーンを表示する
- [ ] #2 音声 ISR の期限を壊さない更新周期にする
<!-- AC:END -->
