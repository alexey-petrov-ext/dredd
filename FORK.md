# Dredd: patch branch for Windows/MSVC

This repository carries changes to [Dredd](https://github.com/mc-imperial/dredd)
that are needed to use it on Windows with MSVC, until they are merged
upstream. It is not a separate product. Every change is meant for upstream,
except the few fork-only files listed below.

## Branches

| Branch | Content |
|---|---|
| `main` | Mirror of `mc-imperial/dredd` `main`. Never commit here. |
| `upstream/<topic>` | One upstream pull request per branch, based on `main`. Each one has tests, is clang-formatted and has nothing fork-specific. |
| `quindos` (default) | `main` + the commits of all `upstream/*` branches + fork-only commits. Releases are tagged here. |

### Changes waiting for upstream

| Branch | Change | Upstream PR |
|---|---|---|
| `upstream/newer-llvm` | Build with LLVM 18–22 and keep LLVM 17; Linux CI job with LLVM 22. Recent MSVC STL releases reject Clang older than 19 (STL1000). | not opened |
| `upstream/ms-compat-dependent-npc` | Fix a crash on value-dependent expressions (e.g. `sizeof(T)` in an instantiated template) with `-fms-compatibility`, the default for MSVC targets. | not opened |
| `upstream/utf8-bom` | Keep a UTF-8 byte order mark at the start of the file; the prelude used to be inserted before it. | not opened |
| `upstream/msvc-ci` (stacked on `upstream/newer-llvm`) | Windows CI with MSVC: build against the official LLVM release, package a portable zip, run the single-file and execute tests and compile every mutated file with `cl.exe`. Includes a cross-platform test runner. See upstream issue #164. | not opened |

### Fork-only files

- `FORK.md` (this file).
- `.github/workflows/fork-linux.yml`: cheap Linux check of the patches with
  LLVM 17 and 22 on every push.
- `scripts/msvc/Install-Dredd.ps1`: installer for the zip package, for the
  repositories that use Dredd.
- Extra triggers in `.github/workflows/msvc.yml` for `quindos` and
  `upstream/**`.

## Updating from upstream

```bash
git remote add upstream https://github.com/mc-imperial/dredd.git   # once
git fetch upstream
old_main=$(git rev-parse main)
old_newer_llvm=$(git rev-parse upstream/newer-llvm)
git checkout main && git merge --ff-only upstream/main
for b in upstream/newer-llvm upstream/ms-compat-dependent-npc upstream/utf8-bom; do
  git rebase main "$b"
done
git rebase --onto upstream/newer-llvm "$old_newer_llvm" upstream/msvc-ci
git rebase --onto main "$old_main" quindos   # replays topic commits + fork-only commits
git push --force-with-lease origin main 'upstream/*' quindos
```

- If git reports a commit as already upstream during the rebase, drop it.
  When a topic is merged upstream, delete its branch and remove its row above.
- Consumers pin release zips (version + SHA-256), not branch commits, so
  rewriting `quindos` does not affect them.
- Exit: when no patches are left, use upstream releases and archive this
  repository.

## Releases

Tag `quindos` with `v<upstream version>-quindos.<n>`, for example
`v1.0-quindos.1`. The `msvc` workflow builds the zip and attaches it and its
`.sha256` to a GitHub release. Builds from other pushes are kept as workflow
artifacts.

`dredd-<version>-windows-x64.zip`:

```
bin/dredd.exe                  Clang/LLVM, protobuf and the C++ runtime linked statically
lib/clang/<major>/include/     Clang's built-in headers (Dredd finds them next to bin/)
scripts/query_mutant_info.py   standard-library Python only
LICENSE, licenses/             Dredd, LLVM and protobuf licences
BUILDINFO.json                 version, commit, LLVM and MSVC toolset
```

Users need neither LLVM nor a Dredd build. Install it with
`scripts/msvc/Install-Dredd.ps1` and a pin file (see the script header). The
install is idempotent and needs no administrator rights.

## Running Dredd on MSVC code

- Run Dredd inside the same Visual Studio developer environment as the build
  (`vcvarsall.bat x64 -vcvars_ver=<toolset>`), so that Clang parses the same
  MSVC headers that `cl.exe` uses. Do not point it at the Clang headers of
  Visual Studio's clang-cl.
- Pass `-fno-delayed-template-parsing` to Dredd, after `--` or through the
  compilation database. Clang's default for MSVC targets leaves templates
  that are never instantiated unparsed, so Dredd would not mutate them.
- Mutated files may trigger C4715 ("not all control paths return a value").
  If the project builds with `/WX`, compile mutants with `_CL_=/WX-`.
- Dredd rewrites files in place, and mutant IDs start from 0 in each Dredd
  run. Mutate a copy of the sources, and give all files of one build to one
  Dredd invocation.

## Building locally

```powershell
scripts\msvc\build.ps1 -LlvmVersion 22.1.8 -VcVarsVer 14.44
scripts\msvc\package.ps1 -Version 0.0.0-local -LlvmDir third_party\clang+llvm-22.1.8-msvc
python scripts\check_tests_with_compiler.py --dredd dist\dredd-0.0.0-local-windows-x64\bin\dredd.exe `
  --style msvc --dredd-std --dredd-arg=-fno-delayed-template-parsing --expectations test\msvc_expectations.txt
```

`build.ps1` downloads the official
`clang+llvm-<version>-x86_64-pc-windows-msvc.tar.xz`, repairs its DIA SDK path
and builds with `/MT`, the runtime that the LLVM libraries use.
