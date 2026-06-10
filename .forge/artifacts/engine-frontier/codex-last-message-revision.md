Done. I created:

- [ultimate-plan.md](<D:/Coding/luminumbra/.forge/artifacts/engine-frontier/ultimate-plan.md>)
- [handoff.md](<D:/Coding/luminumbra/.forge/artifacts/engine-frontier/handoff.md>)
- [dispatch.json](<D:/Coding/luminumbra/.forge/tasks/engine-frontier/dispatch.json>)

Validation passed:

```powershell
forge tasks validate .forge/tasks/engine-frontier/dispatch.json
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Files
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .forge/scripts/validate-engine-frontier.ps1 -Mode Sections
```

I also ran the required CTest scan: `ctest --preset debug --output-on-failure -E "_NOT_BUILT$"` passed 58/58, so no current Wave 1 failing-test repair tasks were added. The dispatch has 34 tasks, starts with `T-EF-1-material-visual-gate`, keeps prompts Codex-only, and has final `T-EF-34-endurance-revalidation`. No commit was made.