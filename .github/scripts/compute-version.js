// SPDX-License-Identifier: GPL-2.0-or-later
//
// Derives the engine's version with no VERSION file and no git tags: fold
// every PR merged to main, in merge order, by its release:{breaking,feature,
// fix,chore} label (release-label-gate.yml guarantees each carries exactly
// one) into an X.Y.Z version. That fold IS the version — recomputed fresh
// every build, not stored anywhere. It's ordinary semver: breaking bumps X,
// feature bumps Y, and fix and chore both bump Z — so every merge, including a
// purely internal one (docs, CI, refactor), still moves the displayed version.
//
// On a pull_request build the PR hasn't merged yet, so there's no final
// version to hand out: fold the already-merged history, then apply this PR's
// own label as a hypothetical last step and mark the result as a prerelease
// for that PR/commit. On a push-to-main build the just-merged PR is already
// in the "closed" list, so no special-casing is needed there.
//
// Run via actions/github-script (see .github/workflows/ci.yml), which
// supplies an authenticated Octokit as `github` and the usual `context`/
// `core` helpers.
module.exports = async ({ github, context, core }) => {
	const { owner, repo } = context.repo;

	const closed = await github.paginate(github.rest.pulls.list, {
		owner,
		repo,
		state: "closed",
		base: "main",
		per_page: 100,
	});

	const merged = closed
		.filter((pr) => pr.merged_at)
		.sort((a, b) => new Date(a.merged_at) - new Date(b.merged_at));

	const bumpOf = (pr) => {
		const label = pr.labels
			.map((l) => l.name)
			.find((n) => n.startsWith("release:"));
		switch (label) {
			case "release:breaking":
				return "major";
			case "release:feature":
				return "minor";
			case "release:fix":
			case "release:chore":
				return "patch";
			default:
				// Shouldn't happen once the label gate is in place: a merged
				// PR with no release:* label at all.
				return "none";
		}
	};

	const fold = ([major, minor, patch], bump) => {
		switch (bump) {
			case "major":
				return [major + 1, 0, 0];
			case "minor":
				return [major, minor + 1, 0];
			case "patch":
				return [major, minor, patch + 1];
			default:
				return [major, minor, patch];
		}
	};

	let version = [0, 0, 0];
	for (const pr of merged) {
		version = fold(version, bumpOf(pr));
	}

	let versionString = version.join(".");

	if (context.eventName === "pull_request") {
		const pr = context.payload.pull_request;
		version = fold(version, bumpOf(pr));
		const shortSha = pr.head.sha.substring(0, 7);
		versionString = `${version.join(".")}-pr${pr.number}+${shortSha}`;
	}

	core.setOutput("version", versionString);
};
