<p align="right">
  <a href="CI-sync-main.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Upstream Synchronization

`.github/workflows/sync-main.yml` synchronizes a fork's `main` branch with `FoloToy/ai-passport:main`. It runs daily at 00:00 UTC and by manual dispatch, and is skipped when the repository is not a fork.

The workflow checks out the target `main` without persisting credentials and uses the full-SHA-pinned upstream-sync Action. Configure the repository secret `UPSTREAM_SYNC_TOKEN` with a fine-grained personal access token that has **Contents: Read and write** and **Workflows: Read and write** access to this fork. The built-in `GITHUB_TOKEN` cannot push changes to `.github/workflows/`, regardless of the workflow's `permissions` setting. If synchronization fails, inspect the Actions log and verify the secret's permissions and expiration.

Keep fork-specific firmware and documentation on feature branches so `main` stays synchronized. If development must happen directly on `main`, disable this workflow first to avoid automatic merges or conflicts. Fork owners must explicitly enable Actions after forking.

The checkout explicitly sets `ref: main`, even when a manual dispatch selects
another branch. The sync Action expects a local `main` branch before fetching;
without that explicit checkout, `git checkout main` can interpret this
repository's `main/` directory as a path instead of switching branches.

When upgrading an Action, verify the release and full commit SHA from the official repository and update the inline version comment.
