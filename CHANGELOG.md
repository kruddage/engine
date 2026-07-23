# Changelog

## [18.8.1](https://github.com/kruddage/engine/compare/v18.8.0...v18.8.1) (2026-07-23)


### Build System

* always fetch the latest kruddage/s7 release, not a pinned tag ([#753](https://github.com/kruddage/engine/issues/753)) ([a1672d9](https://github.com/kruddage/engine/commit/a1672d980c27fd20b3a6d69061cab2a95e2bfc5e))

## [18.8.0](https://github.com/kruddage/engine/compare/v18.7.0...v18.8.0) (2026-07-23)


### Features

* **editor:** show the build version at the toolbar's trailing edge ([#751](https://github.com/kruddage/engine/issues/751)) ([6162f0a](https://github.com/kruddage/engine/commit/6162f0ab06eb4834e2e2e5bda31bc16d2e90c57f))

## [18.7.0](https://github.com/kruddage/engine/compare/v18.6.0...v18.7.0) (2026-07-23)


### Features

* **editor:** color the web shell's version badge yellow when ahead of the last release ([#748](https://github.com/kruddage/engine/issues/748)) ([11232d0](https://github.com/kruddage/engine/commit/11232d09085d1f961ee9c5190910deac17ab2179))
* **editor:** remove the redundant "menu" button from the web shell ([#746](https://github.com/kruddage/engine/issues/746)) ([71e8094](https://github.com/kruddage/engine/commit/71e809489b4a9ed3c2a7024cd4084d0d658a67b5))


### CI

* **flatpak:** rebuild and publish the editor Flatpak on every release ([#750](https://github.com/kruddage/engine/issues/750)) ([66adee2](https://github.com/kruddage/engine/commit/66adee2ba7d4ca820f95e11ff94f5aca7bb036f5))

## [18.6.0](https://github.com/kruddage/engine/compare/v18.5.0...v18.6.0) (2026-07-23)


### Features

* **editor:** collapse the web editor menu bar into a hamburger on narrow viewports ([#744](https://github.com/kruddage/engine/issues/744)) ([f4aa986](https://github.com/kruddage/engine/commit/f4aa986d0e7dc774a7dfd2e46607b27fdbeaf052))

## [18.5.0](https://github.com/kruddage/engine/compare/v18.4.0...v18.5.0) (2026-07-22)


### Features

* **editor:** boot the web editor into the chess scene by default ([#739](https://github.com/kruddage/engine/issues/739)) ([493f6ab](https://github.com/kruddage/engine/commit/493f6ab2c555db693197e0eb456d7aa2deeed9da))
* **editor:** render the web editor chrome from the serialized layout ([#724](https://github.com/kruddage/engine/issues/724)) ([#733](https://github.com/kruddage/engine/issues/733)) ([cfcc98a](https://github.com/kruddage/engine/commit/cfcc98a7b3c67ee715073feb84935344510f6d65))
* **editor:** serialize the .scm layout across the s7→JS seam ([#723](https://github.com/kruddage/engine/issues/723)) ([#732](https://github.com/kruddage/engine/issues/732)) ([fa53c1e](https://github.com/kruddage/engine/commit/fa53c1eeadfc801932e414a9508996f78013d4c4))


### Bug Fixes

* **vulkan:** let the window host declare its instance extensions ([#728](https://github.com/kruddage/engine/issues/728)) ([#729](https://github.com/kruddage/engine/issues/729)) ([c296d75](https://github.com/kruddage/engine/commit/c296d7590ffd5059659b5cbaf24e35c3fa49ae8a))


### Documentation

* **steamos:** surface distrobox requirement before the compiler check fails ([#731](https://github.com/kruddage/engine/issues/731)) ([5dd2d41](https://github.com/kruddage/engine/commit/5dd2d41e4c443b48f95f8627b7f654fc11e35b93))

## [18.4.0](https://github.com/kruddage/engine/compare/v18.3.1...v18.4.0) (2026-07-22)


### Features

* build-only Flatpak packaging pipeline for editor-qt ([#691](https://github.com/kruddage/engine/issues/691)) ([18d7f62](https://github.com/kruddage/engine/commit/18d7f6294eaba50f6fc9fdeb31ac9f919d0c501e))
* **editor:** drive the native Qt chrome from a .scm layout spec ([#722](https://github.com/kruddage/engine/issues/722)) ([#726](https://github.com/kruddage/engine/issues/726)) ([f8b633c](https://github.com/kruddage/engine/commit/f8b633c75493f07be58b1bbb079790cd37b9a448))
* **editor:** lay out the Qt authoring surface with reconfigurable docks ([#676](https://github.com/kruddage/engine/issues/676)) ([#690](https://github.com/kruddage/engine/issues/690)) ([05df362](https://github.com/kruddage/engine/commit/05df3629f8cd244772e72a29e93d0568b96df5ec))
* **editor:** native viewport interaction + real-scene loading ([#697](https://github.com/kruddage/engine/issues/697), [#698](https://github.com/kruddage/engine/issues/698)) ([#725](https://github.com/kruddage/engine/issues/725)) ([223527b](https://github.com/kruddage/engine/commit/223527bc026e164ed86aedb79aee64eb9e53d742))
* **editor:** render the live engine scene in the Qt viewport ([#675](https://github.com/kruddage/engine/issues/675)) ([#696](https://github.com/kruddage/engine/issues/696)) ([488b6b6](https://github.com/kruddage/engine/commit/488b6b679e75bae542e274d92c002e9c0d8c6697))
* **editor:** run the native editor on Vulkan with validation layers ([#708](https://github.com/kruddage/engine/issues/708)) ([6d35d79](https://github.com/kruddage/engine/commit/6d35d7903d40dafc75652281dbc4bd7c8adc4b44))
* **flatpak:** ship AppStream metainfo + icon so the editor lists in Discover ([#710](https://github.com/kruddage/engine/issues/710)) ([bcf8b8f](https://github.com/kruddage/engine/commit/bcf8b8f7e2034b75d1ac8339be96821f888a7125))
* **setup:** add setup.sh to bootstrap the native editor build from source ([#688](https://github.com/kruddage/engine/issues/688)) ([732428f](https://github.com/kruddage/engine/commit/732428fb6f23bae3ce56a12e176ba3092dbdcc0e))
* sign and publish the editor's self-hosted Flatpak registry ([#693](https://github.com/kruddage/engine/issues/693)) ([157d517](https://github.com/kruddage/engine/commit/157d517a5fbfbffda51a21b5b835e00d66debeb8))


### Bug Fixes

* **editor:** misplaced Qt include, plus honest Dawn setup costs ([#699](https://github.com/kruddage/engine/issues/699)) ([2d67b55](https://github.com/kruddage/engine/commit/2d67b5540c779719f6de73ca81210534c861f60c))
* **editor:** report why surface creation failed instead of returning silently ([#727](https://github.com/kruddage/engine/issues/727)) ([f54eff8](https://github.com/kruddage/engine/commit/f54eff8659f6b596e736680acfa95486f463853d))
* **flatpak:** bump org.kde runtime 6.7 -&gt; 6.9 (6.7 is EOL, gone from Flathub) ([#714](https://github.com/kruddage/engine/issues/714)) ([6e2100f](https://github.com/kruddage/engine/commit/6e2100ff56034e2a7cc47798b6d428deb34bac95))
* **flatpak:** install elfutils so eu-strip is available in CI ([#711](https://github.com/kruddage/engine/issues/711)) ([adf4f2e](https://github.com/kruddage/engine/commit/adf4f2e5f9c955494884ee477fe5bf5a50e1f6f0))
* **flatpak:** serialize gh-pages writers so the registry actually publishes ([#712](https://github.com/kruddage/engine/issues/712)) ([4312d1b](https://github.com/kruddage/engine/commit/4312d1bc5252169f46c334d052338eebe3b33718))
* **kruddmake:** compile with -fPIC so the Qt editor stops segfaulting on launch ([#715](https://github.com/kruddage/engine/issues/715)) ([#717](https://github.com/kruddage/engine/issues/717)) ([6d72b8a](https://github.com/kruddage/engine/commit/6d72b8ad50b22735d7defdd61cce91f860ca1ffc))
* **setup:** install a Vulkan driver, not just the loader ([#721](https://github.com/kruddage/engine/issues/721)) ([313d048](https://github.com/kruddage/engine/commit/313d0482a56377f773128b267c9191b11ae1a114)), closes [#701](https://github.com/kruddage/engine/issues/701)
* **webgpu:** release the surface before the device at shutdown ([#692](https://github.com/kruddage/engine/issues/692)) ([a2d8832](https://github.com/kruddage/engine/commit/a2d88320dc408daebe5091472334ccb288de14f1))


### CI

* Bump actions/github-script from 7 to 9 ([#687](https://github.com/kruddage/engine/issues/687)) ([97eb281](https://github.com/kruddage/engine/commit/97eb28157edc00c1829f3a59ce8bd2f124007e57))


### Documentation

* **readme:** add prebuilt Flatpak install above the from-source Deck steps ([#713](https://github.com/kruddage/engine/issues/713)) ([84919ee](https://github.com/kruddage/engine/commit/84919eef2797949f9d932e83a5706299e0d3c8b3))


### Chores

* **s7:** link kruddage/s7 release static libs instead of vendoring s7.c ([#720](https://github.com/kruddage/engine/issues/720)) ([e87708a](https://github.com/kruddage/engine/commit/e87708a8d3d800613f7175ec3e5b497cb332388f)), closes [#707](https://github.com/kruddage/engine/issues/707)

## [18.3.1](https://github.com/kruddage/engine/compare/v18.3.0...v18.3.1) (2026-07-20)


### Documentation

* add native editor (SteamOS / Steam Deck) setup to README ([#684](https://github.com/kruddage/engine/issues/684)) ([92d935b](https://github.com/kruddage/engine/commit/92d935b5c9115c047cb66d88ba47f9b5f9a102ff))

## [18.3.0](https://github.com/kruddage/engine/compare/v18.2.1...v18.3.0) (2026-07-20)


### Features

* **editor:** Qt-hosted WebGPU-native viewport proof of life ([#675](https://github.com/kruddage/engine/issues/675)) ([#683](https://github.com/kruddage/engine/issues/683)) ([e9404bc](https://github.com/kruddage/engine/commit/e9404bc4115b0a445d71f439409f0888a6abfed2))
* **render:** native windowed WebGPU harness for SteamOS proof of life ([#679](https://github.com/kruddage/engine/issues/679)) ([d4b14af](https://github.com/kruddage/engine/commit/d4b14af36325348b46458c552a0f6f99f42e791e))


### CI

* add a tracked pre-commit hook for the .scm lint checks ([#681](https://github.com/kruddage/engine/issues/681)) ([2299e76](https://github.com/kruddage/engine/commit/2299e7636feedf337868985754f30f32f4bd5850)), closes [#680](https://github.com/kruddage/engine/issues/680)

## [18.2.1](https://github.com/kruddage/engine/compare/v18.2.0...v18.2.1) (2026-07-19)


### Bug Fixes

* **shell:** brighten the header version text for legibility ([#659](https://github.com/kruddage/engine/issues/659)) ([0174403](https://github.com/kruddage/engine/commit/01744036d127888e5e56cd6297307430673e80f3))
* **viewport:** restore camera-aspect sync and click-to-pick lost with the editor ([#662](https://github.com/kruddage/engine/issues/662)) ([44ae85a](https://github.com/kruddage/engine/commit/44ae85a8bc38cc8ad1fe129e3d6f9a87ba4b24a9))

## [18.2.0](https://github.com/kruddage/engine/compare/v18.1.0...v18.2.0) (2026-07-19)


### Features

* **chess:** camera zones for turn, selection, and post-move hold ([#658](https://github.com/kruddage/engine/issues/658)) ([d345602](https://github.com/kruddage/engine/commit/d34560279899511cc4f3098858cff067b775af9b))
* **kruddgui:** add a small always-on FPS + frame-time perf HUD ([#646](https://github.com/kruddage/engine/issues/646)) ([99a28cf](https://github.com/kruddage/engine/commit/99a28cff22a31aa415b1d7718e682df4f1cc3d0a))
* **render:** 4x MSAA for the scene via multisampled target + resolve ([#628](https://github.com/kruddage/engine/issues/628)) ([#637](https://github.com/kruddage/engine/issues/637)) ([f3214e8](https://github.com/kruddage/engine/commit/f3214e802831df11072f3f1b10c34a8b1063e932))


### Bug Fixes

* **render:** restore WebGPU rendering broken by the bloom post chain ([#648](https://github.com/kruddage/engine/issues/648)) ([d7a1542](https://github.com/kruddage/engine/commit/d7a15429e9041f5243762becbd0db7e4ee882f56))


### Documentation

* **standard:** add philosophy preamble and a Scheme section ([#647](https://github.com/kruddage/engine/issues/647)) ([#653](https://github.com/kruddage/engine/issues/653)) ([7d09f64](https://github.com/kruddage/engine/commit/7d09f6400040931b871753f8e259b2e735fdb5fc))

## [18.1.0](https://github.com/kruddage/engine/compare/v18.0.0...v18.1.0) (2026-07-19)


### Features

* **chess:** playable moves, ivory subsurface, and in-game selection outline ([#625](https://github.com/kruddage/engine/issues/625)-[#630](https://github.com/kruddage/engine/issues/630)) ([#632](https://github.com/kruddage/engine/issues/632)) ([9c33f98](https://github.com/kruddage/engine/commit/9c33f98b8f50de8f8cc904a53657735a2c34565d))
* **games:** add a chess set showcase game mode ([#621](https://github.com/kruddage/engine/issues/621)) ([921ee07](https://github.com/kruddage/engine/commit/921ee07a58310a2cbd299fa59a2af2a1bf31baaf))
* **render:** add a cheap LDR bloom post chain to the forward path ([#622](https://github.com/kruddage/engine/issues/622)) ([bdcee42](https://github.com/kruddage/engine/commit/bdcee42a40e32ba1b3fd8c1c34ee58c4fb107032))
* **shader:** add an authored emissive term to the pbr material ([#620](https://github.com/kruddage/engine/issues/620)) ([79f02f8](https://github.com/kruddage/engine/commit/79f02f809b0f53b405f92af909d4003a630ccbee))
* **shader:** add reusable functions to the DSL ([#616](https://github.com/kruddage/engine/issues/616)) ([fc38cb8](https://github.com/kruddage/engine/commit/fc38cb8576e85edf59872222d6b5427c2ba9f999))
* **shader:** analytic image-based lighting for the pbr shaders ([#619](https://github.com/kruddage/engine/issues/619)) ([ef4a24e](https://github.com/kruddage/engine/commit/ef4a24e8ac6be8b3431b14b35305efb1d9e40320))
* **shader:** grade the pbr shaders with the ACES filmic tonemap ([#618](https://github.com/kruddage/engine/issues/618)) ([3d6c2e5](https://github.com/kruddage/engine/commit/3d6c2e557a46337dd4d50142dd460e93b2bb5b4a))


### Bug Fixes

* **games:** only run a game's tick while it is the loaded game ([#635](https://github.com/kruddage/engine/issues/635)) ([945a73f](https://github.com/kruddage/engine/commit/945a73f00a7722ca18f5d693cbcdbdabefe82c40))
* **render:** remap WebGPU clip depth and build its mip chain ([#612](https://github.com/kruddage/engine/issues/612)) ([4517980](https://github.com/kruddage/engine/commit/4517980f8201aa8d05bd83b8384dd4cf1d2e4dc8))
* **render:** size the WebGPU bind-group cache for full scenes ([#631](https://github.com/kruddage/engine/issues/631)) ([94c42c3](https://github.com/kruddage/engine/commit/94c42c3a22a94077d1ecfca806e3ad50c2560248))
* **render:** tidy the WebGPU/WebGL port and fix the preview clip depth ([#615](https://github.com/kruddage/engine/issues/615)) ([6717c4b](https://github.com/kruddage/engine/commit/6717c4bf02ba054b9038763789d7b627854322e9))
* **s7:** drop the string_ref_p_p0 swap that traps on wasm ([#633](https://github.com/kruddage/engine/issues/633)) ([4e7d85b](https://github.com/kruddage/engine/commit/4e7d85b1b80de991a2431bd1d94af6c160caad3b))


### Refactoring

* **shader:** share the sun-shadow and tonemap blocks across the built-ins ([#617](https://github.com/kruddage/engine/issues/617)) ([d1f7468](https://github.com/kruddage/engine/commit/d1f7468484ad27a002156f936a1205f9ee01f806))

## [18.0.0](https://github.com/kruddage/engine/compare/v17.16.0...v18.0.0) (2026-07-19)


### ⚠ BREAKING CHANGES

* **render:** texture-native-handle and cmd-bind-texture-native become texture-handle and cmd-bind-texture-handle, and the u32 they trade is no longer a GL texture name.

### Features

* make WebGPU the default renderer, opt out with ?renderer=webgl ([#586](https://github.com/kruddage/engine/issues/586)) ([3c4b96c](https://github.com/kruddage/engine/commit/3c4b96c85b37bf86f0bf2a909756e12c92f5b1f1))
* **render:** add CPU particle system with tic-tac-toe placement effects ([#585](https://github.com/kruddage/engine/issues/585)) ([a7c6e82](https://github.com/kruddage/engine/commit/a7c6e82354273600cbf93889ef67b3d3b7ae079f))
* **render:** bind uniform buffers through a bind group cache ([#583](https://github.com/kruddage/engine/issues/583)) ([6ce3797](https://github.com/kruddage/engine/commit/6ce379703472008523b23d390d448c385f058c13))
* **render:** boot the render cluster on WebGPU and clear its validation errors ([#587](https://github.com/kruddage/engine/issues/587)) ([6618070](https://github.com/kruddage/engine/commit/661807084d0bf9a872b73e88fcf82f64df54c8f7))
* **render:** depth attachments and render-target textures in WebGPU ([#584](https://github.com/kruddage/engine/issues/584)) ([d4323a7](https://github.com/kruddage/engine/commit/d4323a71264344d0654949b6d0bd7de0e15dc908))
* **render:** draw a WGSL triangle in the WebGPU backend ([#570](https://github.com/kruddage/engine/issues/570)) ([38310b6](https://github.com/kruddage/engine/commit/38310b65c110c24db62dc5778b09e6d343574ad2))
* **render:** fuchsia-clear WebGPU probe behind ?renderer=webgpu ([#569](https://github.com/kruddage/engine/issues/569)) ([64f6adb](https://github.com/kruddage/engine/commit/64f6adbddd1a42da6e0dcb044ace256c8b897c0e))
* **render:** give the gpu_api a frame boundary, and put kruddgui on WebGPU ([#599](https://github.com/kruddage/engine/issues/599)) ([4e85801](https://github.com/kruddage/engine/commit/4e85801f4e33c96a90b1cdc3f2f5a9ea1a345301))
* **render:** implement the gpu_api vtable in the WebGPU backend ([#582](https://github.com/kruddage/engine/issues/582)) ([ffdb93e](https://github.com/kruddage/engine/commit/ffdb93ee024c221a1951226e98fbcf3cfbeff175))
* **render:** offscreen native harness for the WebGPU backend ([#594](https://github.com/kruddage/engine/issues/594)) ([0708d1f](https://github.com/kruddage/engine/commit/0708d1ff5cf19a05544594d8ef12af4aff5e4b30))
* **render:** replace the GL-native texture handle with an opaque id ([#597](https://github.com/kruddage/engine/issues/597)) ([f688991](https://github.com/kruddage/engine/commit/f6889917eac62588a291050429addca04e340954))
* **shader:** add fwidth and an opt-in fragment precision to the krudd DSL ([#596](https://github.com/kruddage/engine/issues/596)) ([7846277](https://github.com/kruddage/engine/commit/78462770580ce73592b493463199087fac52775f))
* **shader:** lower the shader DSL to WGSL for the WebGPU backend ([#568](https://github.com/kruddage/engine/issues/568)) ([a1a0c5a](https://github.com/kruddage/engine/commit/a1a0c5a376729c898d7c60e41c2bd87d92424222))
* **shell:** detect WebGPU on load and badge the active renderer in the header ([#565](https://github.com/kruddage/engine/issues/565)) ([a4e7d89](https://github.com/kruddage/engine/commit/a4e7d8911c733a9ee3ff9cf42a33433839fc4f3b))
* **shell:** show the launcher on WebGPU, and stop shipping the probe log ([#611](https://github.com/kruddage/engine/issues/611)) ([25b8587](https://github.com/kruddage/engine/commit/25b85877f07d2b4c930b6cf6e979f24e071610a6)), closes [#572](https://github.com/kruddage/engine/issues/572)


### Bug Fixes

* lock Firefox to WebGL, make the renderer badge clickable to switch ([#589](https://github.com/kruddage/engine/issues/589)) ([d083383](https://github.com/kruddage/engine/commit/d0833839b7a1192a16843b7539f1b11b3bd140a9))
* **render:** free write-only frame-graph transients each frame ([#588](https://github.com/kruddage/engine/issues/588)) ([bf4397e](https://github.com/kruddage/engine/commit/bf4397eadd0d44545675b40b4567334550df08cd))
* **render:** give each draw its own uniform slot, not one shared UBO ([#602](https://github.com/kruddage/engine/issues/602)) ([a354690](https://github.com/kruddage/engine/commit/a3546908371ffc519661a0b79e7dbf81c85f69d9))
* **render:** make the fallback shadow map a depth texture on WebGPU ([#609](https://github.com/kruddage/engine/issues/609)) ([68ddf80](https://github.com/kruddage/engine/commit/68ddf80d8d9b3bd182216d1e125829ffac56e84c)), closes [#578](https://github.com/kruddage/engine/issues/578) [#572](https://github.com/kruddage/engine/issues/572)
* **render:** map the WebGPU shadow projection into [0,1] clip depth ([#608](https://github.com/kruddage/engine/issues/608)) ([da4c38d](https://github.com/kruddage/engine/commit/da4c38dab7ddfc4299d31917df9b41afff82880a)), closes [#604](https://github.com/kruddage/engine/issues/604) [#606](https://github.com/kruddage/engine/issues/606)
* **render:** size the WebGPU backbuffer in device pixels, not CSS pixels ([#610](https://github.com/kruddage/engine/issues/610)) ([91154be](https://github.com/kruddage/engine/commit/91154be909f90b1a491f1a33a8127943cc5b3edc)), closes [#572](https://github.com/kruddage/engine/issues/572)
* **ui:** only resize the canvas when its size actually changed ([#595](https://github.com/kruddage/engine/issues/595)) ([9941584](https://github.com/kruddage/engine/commit/9941584bc491e2659948a0f992ae6aad1a8fa6c2))


### Build System

* **render:** compile the WebGPU backend natively behind a platform seam ([#593](https://github.com/kruddage/engine/issues/593)) ([4c34129](https://github.com/kruddage/engine/commit/4c341292212f4ec6471da5a405a571659d26d30d))
* **tools:** native Dawn build seam + offscreen smoke binary ([#591](https://github.com/kruddage/engine/issues/591)) ([4ac5077](https://github.com/kruddage/engine/commit/4ac507739eb1c9e5b2fad3a04cda98b2ba77dd06))


### CI

* add native sanitizer gate and coverage report ([#592](https://github.com/kruddage/engine/issues/592)) ([00c5db7](https://github.com/kruddage/engine/commit/00c5db78da6aa66495e82b6a050ffd66c2854dff))
* don't let the preview deploy job block auto-merge ([#601](https://github.com/kruddage/engine/issues/601)) ([c343ffc](https://github.com/kruddage/engine/commit/c343ffcc6d6c6b7358f96f06ec9a377211478433))


### Tests

* **render-diff:** add the WebGPU-vs-WebGL parity scene ([#598](https://github.com/kruddage/engine/issues/598)) ([6205d9f](https://github.com/kruddage/engine/commit/6205d9f71bd455327b8b01f582b8c428972e5006))
* **render-diff:** hide the shell's overlays before capturing ([#600](https://github.com/kruddage/engine/issues/600)) ([12a5cb5](https://github.com/kruddage/engine/commit/12a5cb5ede747c088746e4caf2ba2992090d9b44))
* **render-diff:** make capture deterministic to kill the parity noise floor ([#607](https://github.com/kruddage/engine/issues/607)) ([2f368a6](https://github.com/kruddage/engine/commit/2f368a65fe4610ce302e0d0fe857b8cade89b70f)), closes [#603](https://github.com/kruddage/engine/issues/603)


### Chores

* **tools:** add a render-diff harness for the WebGPU port ([#581](https://github.com/kruddage/engine/issues/581)) ([e9ceb80](https://github.com/kruddage/engine/commit/e9ceb806cb303b4dc6bf80e462ae58979451067b))

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
