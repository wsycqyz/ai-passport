<p align="right">
  <strong>简体中文</strong> · <a href="CI-sync-main.md">English</a>
</p>

# 上游同步（CI / Upstream Sync）

本仓库提供一套基于 GitHub Actions 的自动上游同步流水线，用于定期把上游 `FoloToy/ai-passport` 的 `main` 分支更新同步到本 fork 的 `main` 分支。

本文件与 `.github/workflows/sync-main.yml` 一同维护，工作流行为变化时必须同步更新。

## 触发条件

- **schedule**：每天 00:00（UTC）自动运行一次。
- **workflow_dispatch**：可在 GitHub Actions 页面手动触发（用于立即同步/排查问题）。

> 该工作流仅在仓库为 **fork** 时生效（`if: github.event.repository.fork`）；非 fork 仓库不运行。

## 流水线做了什么

1. **Checkout 目标仓库**：`actions/checkout` 显式设置 `ref: main`，即使手动触发选择其他分支，也检出当前 fork 的 `main`，并关闭 Git 凭证持久化。同步 Action 在 fetch 前需要本地 `main`；否则 `git checkout main` 可能把仓库内的 `main/` 目录作为路径处理，并未切换分支。
2. **同步上游**：使用固定到完整 commit SHA 的 `aormsby/fork-sync-with-upstream-action`（对应 v3.4.3），把 `FoloToy/ai-passport` 的 `main` 同步到本 fork 的 `main`。请配置仓库密钥 `UPSTREAM_SYNC_TOKEN`，使用对本 fork 具有 **Contents: Read and write** 和 **Workflows: Read and write** 权限的细粒度个人访问令牌。无论工作流的 `permissions` 如何设置，自动生成的 `GITHUB_TOKEN` 都不能推送对 `.github/workflows/` 的更改。
3. **失败检查**：同步失败时输出提示，检查 Actions 日志并确认 `UPSTREAM_SYNC_TOKEN` 的权限和有效期。

## 注意事项

- 同步目标与上游分支均为 `main`，与 fork 用户约定（`main` 仅允许修改根目录 `README.md` 与 `docs/assets/`）配合使用：`main` 保持与上游最新基线同步、不产生冲突。
- **在 `main` 直接开发的例外**：如果用户执意要在 `main` 分支直接开发，必须**停用/关闭本 workflow**（Actions 页面 → Disable），否则每日自动同步会把上游改动强行合入 `main`，产生冲突或覆盖本地开发内容。
- 若同步失败，查看 Actions 日志并确认 `UPSTREAM_SYNC_TOKEN` 已配置，且具有所需权限且未过期。
- 升级 Action 时必须从官方仓库核对目标版本对应的完整 commit SHA，并同步更新 workflow 行尾的版本注释。

## 相关文件

- `.github/workflows/sync-main.yml`：本流水线定义。
