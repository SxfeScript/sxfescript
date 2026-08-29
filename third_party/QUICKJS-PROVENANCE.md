# QuickJS provenance

- Fork name: **ArcSX** (this repository's own quickjs fork, diverged from the
  snapshot below with local SX-language patches and upstream cherry-picks)
- Upstream project: `quickjs-ng/quickjs`
- Rayact fork URL: `https://github.com/raythings/quickjs.git`
- Exported Git revision: `66f4965`
- Rayact parent checkpoint: `c8944aa`
- Export date: 2026-08-28

The Raythings remote was unavailable when this standalone repository was
created, so the committed tree was exported with `git archive` and is tracked
directly. QuickJS retains its MIT license in `third_party/quickjs/LICENSE`.

To refresh the snapshot, first commit and test the desired QuickJS revision,
archive only its tracked tree, replace this directory while preserving SXFE
changes as a patch series, and update this document and the compatibility tests.

