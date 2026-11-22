---
id: TASK-1
title: テンション選択を順送りからフレーズ文法へ変える
status: To Do
assignee: []
created_date: '2026-11-22T15:46:00Z'
labels: []
dependencies: []
references:
  - https://github.com/divergen371/frdm_mcxn947_ambient_fx/issues/1
documentation:
  - .dev/contexts/feat-dsp-cycle-profiling.md
priority: high
type: feature
ordinal: 1000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
P9 / P10 のテンションはカウンタで 4 種を順に回している。同じストローク列が毎回同じ順になる。同じ入力でも繰り返しが一定でないことは歓迎で、増やしたいのは種類の数より次に何を出すかの判断。

NPU とスペクトル重心・密度は判断の本体にしない。アタック検出、開花のスムージング、ピッチ処理は CPU のまま残す。
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 カウンタ (voicing + 1) & 3 を廃止し、直前のボイシングを覚えた短い文法で次を選ぶ
- [ ] #2 直前と同じボイシングは基本的に避け、4 回に 1 回だけ動機として残せる
- [ ] #3 #11 や m7 のあとには add9 かシマーを一段薄くする休符へ解決する
- [ ] #4 sus11 は解決の前に置き、連続させない
- [ ] #5 既存のゆっくりした LFO 位相を重みに足し、乱数テーブルは使わない
- [ ] #6 P9 にオクターブ休符と一度だけの #11、P10 に一度引いてから戻るを足し、8 種の順送りにはしない
<!-- AC:END -->
