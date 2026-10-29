# Daily Windows runner

`windows-daily.yml` runs the full MSVC and MSYS2 UCRT64 builds, Python contracts,
two CTest shards per toolchain and their inventory aggregates on the user's
Windows machine. Its one daily cron is 10:17 UTC (05:17 US Eastern standard time,
06:17 daylight time). GitHub schedules load the workflow file from the default branch, where this
workflow must also be present. Each scheduled run resolves `devel` once and tests
that immutable SHA in every job, regardless of the repository default branch.
Normal CI retains its cheap hosted Windows runner, asset and Python checks.

The repository runner needs labels `[self-hosted, windows, x64, lum-windows]`.
Install Git for Windows with Bash and cygpath, PowerShell >= 7.4, CMake/CTest >= 3.21,
Ninja, 7-Zip, GitHub CLI (`gh`), and Visual Studio with the x64 MSVC C++ tools,
Windows SDK and vswhere. Make these tools available to the service account.
CTest 3.21 provides JUnit output; PowerShell 7.4 propagates native command errors.
The previous jobs did not pin exact versions for those tools. The workflow
selects CPython **3.14.8 x64**, stages checksum-pinned MSVC ccache **4.14.1** and
Mesa3D llvmpipe **26.1.7**, and installs/updates the original MSYS2 UCRT64 packages
(Git, ccache, CMake, GCC, Ninja, Python, numpy and Pillow). MSYS2 package versions
remain rolling, as before. Network access for actions, packages and dependencies
is needed on the runner. `tools/ci/windows_runner_preflight.py` verifies tools
and names missing prerequisites before configuration; it never builds anything.
Checkouts clean the persistent worktree. Separate 1200M ccache directories live
under the service account's LocalAppData, outside the worktree and job temp.
The aggregate jobs expose the selected CPython interpreter to Bash as `python3`;
MSVC standalone contracts enter the same x64 developer environment as the build.
Existing 90/85-minute build and five-minute aggregate limits remain unchanged.

Follow GitHub's official instructions for
[adding a repository self-hosted runner](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/adding-self-hosted-runners)
and [configuring it as a service](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/configuring-the-self-hosted-runner-application-as-a-service).
The user creates the registration token in repository **Settings > Actions >
Runners** and enters it locally; it is never committed. Add `--labels lum-windows`
to the local configuration command; the runner supplies the other default labels.
Run the service under a dedicated non-admin local account. Installing the service
itself requires an elevated setup session. No registration script or credentials
are stored in this repository.

Trigger on demand with `gh workflow run windows-daily.yml` (optionally `--ref`
with a branch or tag). For a same-repository pull request, add the `windows-proof`
label. Labeled, synchronize and reopened events run only while that label is
present; fork PRs cannot reach any self-hosted job. Draft rejection remains in
place. Manual and scheduled runs queue without cancelling each other; a PR run
cancels only older daily proof runs for that PR. See
[manual qualification capture](ci-qualification-capture.md) for binary-bound evidence.

A red scheduled or manual run creates or updates **Daily Windows run**, labeled
`windows-daily`, with a failure comment naming the run URL, head SHA and failed
jobs. A later passing run comments and closes it. The report uses only the
workflow token through `gh`. PR proof runs do not update the issue. Red Windows
proof gates the **v0.3.1 tag**, not landings, and never freezes `devel`. This change
uses the user's runner and adds no paid or larger GitHub runner.
