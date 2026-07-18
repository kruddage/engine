# Changelog

## [17.16.0](https://github.com/kruddage/engine/compare/v17.15.0...v17.16.0) (2026-07-18)


### Features

* **entity:** build a world from a (scene ...) Scheme form + tic-tac-toe game ([#551](https://github.com/kruddage/engine/issues/551)) ([6e0833b](https://github.com/kruddage/engine/commit/6e0833baf38c0ff3896a4eedcf35e06efffe6eae))
* **entity:** dispatch clicks to Scheme game rules; playable tic-tac-toe placement ([#554](https://github.com/kruddage/engine/issues/554)) ([be917f3](https://github.com/kruddage/engine/commit/be917f36dc9df6e682c9d90023019d0b64a06496))
* **entity:** nest scene entities via a (children ...) clause; real tic-tac-toe board ([#553](https://github.com/kruddage/engine/issues/553)) ([202dff9](https://github.com/kruddage/engine/commit/202dff9f042f870ee15b369fec9bed31d040d7f1))
* **game:** hide the editor chrome in tic-tac-toe's play view ([#558](https://github.com/kruddage/engine/issues/558)) ([176dfa2](https://github.com/kruddage/engine/commit/176dfa2c9e96f6780ba6f4535d662a6dca9f46aa))
* **game:** launcher registry + HTML menu to pick a scene at boot ([#556](https://github.com/kruddage/engine/issues/556)) ([bc8e318](https://github.com/kruddage/engine/commit/bc8e318a63fc88e13f0b0e242fd40b83a5132b69))
* **shell:** add a close button to the scene launcher ([#561](https://github.com/kruddage/engine/issues/561)) ([8fdf0f8](https://github.com/kruddage/engine/commit/8fdf0f82f672e93d037b568f5cfcf285d917ca63))
* **tictactoe:** gem materials, grassy heightfield ground, and sound ([#564](https://github.com/kruddage/engine/issues/564)) ([07ebb98](https://github.com/kruddage/engine/commit/07ebb98ae9a789d5254bf814c730d2a9f7c5ff18))
* **tictactoe:** strike-through win line and 1P/2P scoreboard ([#559](https://github.com/kruddage/engine/issues/559)) ([d507257](https://github.com/kruddage/engine/commit/d5072578f9c824abb7d87283643908a4b81aa7b2))
* **tictactoe:** win/draw detection and click-to-restart ([#555](https://github.com/kruddage/engine/issues/555)) ([4be850f](https://github.com/kruddage/engine/commit/4be850f6a8028f49c1068b6695c2af2968db0815))


### Bug Fixes

* **game:** give each scene its own camera and gate the launcher until ready ([#557](https://github.com/kruddage/engine/issues/557)) ([5e58d93](https://github.com/kruddage/engine/commit/5e58d936de6b1dea6a9929d63cf11d310b8ec383))
* **kruddgui:** don't fire a tap at the end of a scroll drag ([#562](https://github.com/kruddage/engine/issues/562)) ([2402b20](https://github.com/kruddage/engine/commit/2402b202f56604a9cfa4e7889d5200da26e54c79))
* **ui:** keep the live viewport visible under an open kruddgui console ([#563](https://github.com/kruddage/engine/issues/563)) ([98972ba](https://github.com/kruddage/engine/commit/98972ba0e8381f1abe5d3f7060b211a24a2fa769))

## [17.15.0](https://github.com/kruddage/engine/compare/v17.14.0...v17.15.0) (2026-07-17)


### Features

* **mesh:** add an SDF + marching-cubes shape engine and an sdf-rook built-in ([#547](https://github.com/kruddage/engine/issues/547)) ([39af780](https://github.com/kruddage/engine/commit/39af780970145166805c47cbeaafe099e4fb217e))
* **render:** shadow-mapped sun light ([#548](https://github.com/kruddage/engine/issues/548)) ([7acc670](https://github.com/kruddage/engine/commit/7acc6705017a4829348c0cc240843f0140d927ba))

## [17.14.0](https://github.com/kruddage/engine/compare/v17.13.0...v17.14.0) (2026-07-17)


### Features

* **audio:** add a Play button to sound assets in the inspector ([#545](https://github.com/kruddage/engine/issues/545)) ([b44b47d](https://github.com/kruddage/engine/commit/b44b47dfc9deeb9eb30ba7a8c02fed04bb437db9))
* **audio:** mono bake + the voice mixer (playback core) ([#540](https://github.com/kruddage/engine/issues/540)) ([0f83148](https://github.com/kruddage/engine/commit/0f8314865da0609601c0cd271e9e42ebc105e3a6))
* **audio:** ScriptProcessorNode backend driving the mixer ([#543](https://github.com/kruddage/engine/issues/543)) ([c3aadcc](https://github.com/kruddage/engine/commit/c3aadcc84ca180c98de9e10a111d9e8486d178a4))
* **gizmo:** finger-first grab targets + resolution-relative sensitivity ([#534](https://github.com/kruddage/engine/issues/534)) ([#535](https://github.com/kruddage/engine/issues/535)) ([0876c06](https://github.com/kruddage/engine/commit/0876c06fa64f674a6f5ecbaa74f4318c0092649a))
* **kruddgui:** bigger, shared console header touch targets ([#537](https://github.com/kruddage/engine/issues/537)) ([cd1eb06](https://github.com/kruddage/engine/commit/cd1eb06d6a610d3a1a7a4d6458133455ce4259a2))
* **render:** physically based (metallic-roughness) materials ([#544](https://github.com/kruddage/engine/issues/544)) ([2ff6bbb](https://github.com/kruddage/engine/commit/2ff6bbb4fb14e0d93a7690e563a8ac04fa273b88))
* **render:** scene light entities drive the pbr directional light ([#546](https://github.com/kruddage/engine/issues/546)) ([6d52b8f](https://github.com/kruddage/engine/commit/6d52b8fb359c73337b5b611fe10935bb5f22b158))
* **sound:** procedural sound assets baked from Scheme scripts ([#539](https://github.com/kruddage/engine/issues/539)) ([99b71c6](https://github.com/kruddage/engine/commit/99b71c641418f4a42f4aaa93d707a83e10b4c1cb))


### Refactoring

* route kruddboard texture preview through gpu_api ([#541](https://github.com/kruddage/engine/issues/541)) ([856ad9f](https://github.com/kruddage/engine/commit/856ad9f994a8b0433391acebe52b1f766df1f08a))


### Chores

* remove dead code — DAG spike, imgui fetch, renderer_interface, krudd-fetch ([#538](https://github.com/kruddage/engine/issues/538)) ([a152cab](https://github.com/kruddage/engine/commit/a152cabee0194cc7132893433a3d99aca380d62a))

## [17.13.0](https://github.com/kruddage/engine/compare/v17.12.0...v17.13.0) (2026-07-16)


### Features

* dock layout shell for kruddgui panels ([#531](https://github.com/kruddage/engine/issues/531)) ([eba71e6](https://github.com/kruddage/engine/commit/eba71e6d30ad24e4e7854c286617a1fdbbe43884))
* **kruddgui:** render text from an SDF atlas (JetBrains Mono) ([#533](https://github.com/kruddage/engine/issues/533)) ([9671020](https://github.com/kruddage/engine/commit/9671020a3177fa14a320044be3606ca17d9e0e2e))

## [17.12.0](https://github.com/kruddage/engine/compare/v17.11.3...v17.12.0) (2026-07-15)


### Features

* **engine:** remove the imgui subsystem — kruddgui stands alone ([#492](https://github.com/kruddage/engine/issues/492)) ([#530](https://github.com/kruddage/engine/issues/530)) ([4f75511](https://github.com/kruddage/engine/commit/4f75511e68c7e8400c878c4479b941112094b34a))
* **kruddboard:** port the transform gizmo + pick onto kruddgui ([#492](https://github.com/kruddage/engine/issues/492)) ([#529](https://github.com/kruddage/engine/issues/529)) ([714469f](https://github.com/kruddage/engine/commit/714469ff6847393cd524214a265c67d9363597b0))
* **kruddgui:** add an image primitive to the quad batch ([#492](https://github.com/kruddage/engine/issues/492)) ([#520](https://github.com/kruddage/engine/issues/520)) ([087692c](https://github.com/kruddage/engine/commit/087692cbd8ef17c57f4493b459389dd6b6319897))
* **kruddgui:** add fold header + button row for the Assets lift ([#492](https://github.com/kruddage/engine/issues/492)) ([#516](https://github.com/kruddage/engine/issues/516)) ([7c25c02](https://github.com/kruddage/engine/commit/7c25c02a4adab6535916de282f3558a26b7b7617))
* **kruddgui:** add line/circle/ring primitives to the quad batch ([#492](https://github.com/kruddage/engine/issues/492)) ([#527](https://github.com/kruddage/engine/issues/527)) ([26f4444](https://github.com/kruddage/engine/commit/26f444401bad9694136e437f67e72263eba2aed5))
* **kruddgui:** add the multiline text field primitive ([#492](https://github.com/kruddage/engine/issues/492)) ([#523](https://github.com/kruddage/engine/issues/523)) ([a7abd25](https://github.com/kruddage/engine/commit/a7abd25693d8c10c5099f2aa164d48fee7e93bf5))
* **kruddgui:** add the viewport overlay seam (kruddgui_api) ([#492](https://github.com/kruddage/engine/issues/492)) ([#528](https://github.com/kruddage/engine/issues/528)) ([5c6ceb5](https://github.com/kruddage/engine/commit/5c6ceb5e2118d7d669e8edf3bd10531077d454c4))
* **kruddgui:** port the Assets browser onto kruddgui ([#492](https://github.com/kruddage/engine/issues/492)) ([#518](https://github.com/kruddage/engine/issues/518)) ([8e01b12](https://github.com/kruddage/engine/commit/8e01b12ab3039f54a253dd3ee02d97cb042ef0c1))
* **kruddgui:** port the markdown preview off ImGui onto kruddgui ([#492](https://github.com/kruddage/engine/issues/492)) ([#526](https://github.com/kruddage/engine/issues/526)) ([32f85be](https://github.com/kruddage/engine/commit/32f85be262b4f25756ddcd47f1d22754b846f342))
* **kruddgui:** port the material editor onto kruddgui ([#492](https://github.com/kruddage/engine/issues/492)) ([#519](https://github.com/kruddage/engine/issues/519)) ([2dd1944](https://github.com/kruddage/engine/commit/2dd1944e67573f2e10e7d25e7c237f83093f090f))
* **kruddgui:** port the mesh editor onto kruddgui ([#492](https://github.com/kruddage/engine/issues/492)) ([#522](https://github.com/kruddage/engine/issues/522)) ([669837c](https://github.com/kruddage/engine/commit/669837c02f57c235c37c4d19d15881199d0651f3))
* **kruddgui:** port the texture editor onto kruddgui ([#492](https://github.com/kruddage/engine/issues/492)) ([#521](https://github.com/kruddage/engine/issues/521)) ([4e29044](https://github.com/kruddage/engine/commit/4e29044fad8ec66f245d593f6353cb6865c01ceb))
* **kruddgui:** retire the ImGui board window for a kruddgui toolbar ([#492](https://github.com/kruddage/engine/issues/492)) ([#525](https://github.com/kruddage/engine/issues/525)) ([f8a548c](https://github.com/kruddage/engine/commit/f8a548c8c45512ea5c776be7bed07b5afd829260))
* **kruddgui:** wire the source editors onto kgui-field-multi ([#492](https://github.com/kruddage/engine/issues/492)) ([#524](https://github.com/kruddage/engine/issues/524)) ([c98de6a](https://github.com/kruddage/engine/commit/c98de6afe4baed6d010aca385352bf1c947d1855))


### Build System

* **kruddmake:** track codegen deps so `.scm` edits rebuild consumers ([#492](https://github.com/kruddage/engine/issues/492)) ([#514](https://github.com/kruddage/engine/issues/514)) ([e482ffa](https://github.com/kruddage/engine/commit/e482ffa33a06114fbd4578cefcde0b0e7e6ce5b8))


### CI

* bump amannn/action-semantic-pull-request from 5 to 6 ([#513](https://github.com/kruddage/engine/issues/513)) ([456318c](https://github.com/kruddage/engine/commit/456318c0554b680bda590a15814eaad2c84a8a01))
* bump googleapis/release-please-action from 4 to 5 ([#512](https://github.com/kruddage/engine/issues/512)) ([012ac05](https://github.com/kruddage/engine/commit/012ac0568b6634e06388e44144e49106f9d4d19e))
* switch versioning from release:* labels to release-please ([#511](https://github.com/kruddage/engine/issues/511)) ([5aee768](https://github.com/kruddage/engine/commit/5aee768798bbe368d4cd5d42fe6223305a75f8ba))
