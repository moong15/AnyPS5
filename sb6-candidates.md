# sb6 fps wins and PR candidates (overnight 2026-11-14)

Keep rule: 2 runs, both beat previous sb6 run (presents per 90 s window).

| Change | Source | Commit | Runs (presents) | vs prev | Kept | PR-worthy |
|---|---|---|---|---|---|---|
| Wake dormant q0 after queued commits | own, upstream PR #20132 | 1455bee48a | 828, 903 (run821-822) | - | yes (fix) | already PR #20132 |

## Upstream PR candidates (Haiku search, list stale: prtrack hit 1000-PR cap, list as of 2026-11-13 14:00Z)
| PR | What | Port state |
|---|---|---|
| #18757 | learn draw relocation at insert | KEPT: run830 887, run831 924 (+2.5%, +6.8% vs mean 865) on perf/sb6; PR candidate: upstream PR already open, add evidence comment? (needs user OK) |
| #15674 | Refresh lookups from index+cache (target lookup) | KEPT: run832 910, run833 887 (+5.2%, +2.5%) on perf/sb6; upstream PR open |
| #16115 | reuse open render pass attachments | DROPPED: run834 885, run835 852 |
| #15659 | keep image table cache entries | KEPT: run836 869, run837 915 (+0.5%, +5.8%) on perf/sb6 |
| #18102 | collect memo reuse when results pending | KEPT: run838 996, run839 952 (+15%, +10%) on perf/sb6 |
| #17786 | shared collect walks (16 conflicts) | later |
| #17738, #16753, #17943 | large (target memo, per-draw fixed cost, record encoder thread) | later, high risk |
| twalk: per-texture collect proof | own (Sol) | 542f385e1d | run840 876, run841 907 | +1.3%, +4.9% | yes | yes, own PR candidate (needs docs row + tests + main-vs-PR pairs) |
| dlookup: draw key memo | own (Sol) | port/sb6-dlookup | alone 968, 902; in stack off-runs 924, 943 beat stack mean 923 | no gain in stack | DROPPED from stack | no |

Bar for candidates: both runs above baseline mean 865 (run821 828, run822 903).
| drel: dispatch read-base relocation (opt-in) | own (Sol) | on perf/sb6 | run880 963, run881 940 | +3.9%, +1.4% vs 927 | yes | yes, own PR candidate (opt-in, partial; tests pending) |
| #17943 encoder thread port | upstream #17943 | 83d680a251 port/sb6-enc | queued after chain | - | - | upstream |
| #16753 per-draw fixed cost port | upstream #16753 | f31cd5955b | queued chain-b | - | - | upstream |
| #17786 shared collect walks port | upstream #17786 | a6b343a12d | queued chain-b | - | - | upstream; conflicts with twalk |
| #18287 vertex-plan scratch (merged on main) | main 02f091a37d | cc4a705c9c | queued chain-b | - | - | already merged |
| #18914 PM4 CPU work port | upstream #18914 | 4f83f52da9 | queued chain-b | - | - | upstream |
| #20041 epoch proofs follow committer | upstream #20041 | 472b94ddd9 | queued chain-b | - | - | upstream |
| #19978 shared read ranges | upstream #19978 | e09ee7637e | queued chain-b | - | - | upstream |
| #17738 target memo | upstream #17738 | 71e0b3cf79 | queued chain-b | - | - | upstream |
| #20130 data-only stage reuse | upstream #20130 | 066623107f | queued separately (cold cache, warm first) | - | - | upstream |

Stack (perf/sb6 542f385e1d = base + #18757 #15674 #15659 #18102 twalk): bar for next batch = 927 (pooled stack runs 870/871/878 + dlookup-off 873/876).
| #17943 result | | | run882 917, run883 921 | below 927 | DROPPED | |
| #16753 result | | | run884 919, run885 931 | mixed | DROPPED | |
| drel2: descriptor table relocation | own (Sol) | 51711a9780 | queued | - | - | maybe |
| #17786 result | | | run886 957, run887 903 | mixed | DROPPED | |
| #18287 result | | | run888 934, run889 928 | +0.8%, +0.1% | KEPT (marginal) | already merged on main |
| twalk PR prep | | pr/target-proof-memo 17a1900f56 | | | | PR-ready pending main-vs-PR runs; needs user OK |
| #18914 result | | | run890 897, run891 909 | below | DROPPED | |
| drel PR prep | | pr/dispatch-relocation 098c36d763 | | | | PR-ready pending main-vs-PR runs; needs user OK |
| #20041 result | | | run892 965, run893 977 | +4.1%, +5.4% | KEPT | upstream PR open (evidence comment needs user OK) |
| #19978 result | | | run894 916, run895 881 | below | DROPPED | |
| #17738 result | | | run896 944, run897 930 | +1.8%, +0.3% | KEPT (marginal) | upstream PR open |

Stack2 (perf/sb6 86ef130af6): run900 978, run901 956, mean 967 = +11.8% vs original base 865. New bar 967.
