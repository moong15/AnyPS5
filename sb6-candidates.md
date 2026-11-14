# sb6 fps wins and PR candidates (overnight 2026-11-14)

Keep rule: 2 runs, both beat previous sb6 run (presents per 90 s window).

| Change | Source | Commit | Runs (presents) | vs prev | Kept | PR-worthy |
|---|---|---|---|---|---|---|
| Wake dormant q0 after queued commits | own, upstream PR #20132 | 1455bee48a | 828, 903 (run821-822) | - | yes (fix) | already PR #20132 |

## Upstream PR candidates (Haiku search, list stale: prtrack hit 1000-PR cap, list as of 2026-11-13 14:00Z)
| PR | What | Port state |
|---|---|---|
| #18757 | learn draw relocation at insert | KEPT: run830 887, run831 924 (+2.5%, +6.8% vs mean 865) on perf/sb6; PR candidate: upstream PR already open, add evidence comment? (needs user OK) |
| #15674 | Refresh lookups from index+cache (target lookup) | ported port/sb6-15674 8c2624a02e, queued in chain-sb6c |
| #16115 | reuse open render pass attachments | ported dd8301a39c, queued |
| #15659 | keep image table cache entries | ported 8cbc3e24a8, queued |
| #18102 | collect memo reuse when results pending | ported 07ff0cd23f, queued |
| #17786 | shared collect walks (16 conflicts) | later |
| #17738, #16753, #17943 | large (target memo, per-draw fixed cost, record encoder thread) | later, high risk |
| twalk: per-texture collect proof | own (Sol) | c6e062131b | queued 840+ | - | - | maybe |
| dlookup: draw key memo | own (Sol) | 1ec4940a5a | queued 850+ | - | - | maybe |

Bar for candidates: both runs above baseline mean 865 (run821 828, run822 903).
| drel: dispatch read-base relocation (opt-in) | own (Sol) | 78a8ca2dff | queued after chain | - | - | maybe |
| #17943 encoder thread port | upstream #17943 | 83d680a251 port/sb6-enc | queued after chain | - | - | upstream |
