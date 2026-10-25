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
| `upstream/prelude-without-stl` | Mutated C++ code calls no function of the C++ standard library: the prelude reads `DREDD_ENABLED_MUTATION` with `std::strtol`, and mutator functions take lazy operands as a non-owning `__dredd_lazy<T>`, not `std::function`. A program that links another variant of that library, such as `/MTd` code with the release C runtime, then still links when mutated. | not opened |
| `upstream/tracking-prelude-without-stl` (stacked on `upstream/prelude-without-stl`) | The C++ prelude of `--only-track-mutant-coverage` also calls no function of the C++ standard library: it appends to the tracking file with `std::fopen` and `std::fprintf`, and marks recorded mutants with the atomic exchange of a compiler intrinsic, not `std::atomic<bool>`. The reach build of `/MTd` code that links the release C runtime then links too. Both tracking preludes, C and C++, record each mutant once for each process, and serialize the appends with one spin lock for the whole program, so that threads do not overwrite each other's lines (the C runtime of Windows emulates append mode). | not opened |
| `upstream/msvc-ci` (stacked on `upstream/newer-llvm`) | Windows CI with MSVC: build against the official LLVM release, package a portable zip, run the single-file and execute tests and compile every mutated file with `cl.exe`. Includes a cross-platform test runner. See upstream issue #164. Open it only after `upstream/ms-compat-dependent-npc` is merged: without that fix, Dredd crashes on `sizeof_template.cc` and `sizeof_template2.cc` in the Windows job. | not opened |
| `upstream/ignore-comments` | A line comment `// Dredd disable once <names>: <reason>`, or a `disable` … `restore` block, marks mutants as ignored, so that a mutation analysis script can leave out a mutant that no test can kill. The mutation info file lists them under `ignoredMutations`. Dredd still applies them, so the comments change neither the mutant IDs nor the mutated code. | not opened |
| `upstream/prelude-thread-local-keyword` (stacked on `upstream/tracking-prelude-without-stl`) | The mutant prelude no longer defines the keyword `thread_local` as a macro (`__declspec(thread)` for MSVC, `__thread` for Apple): its own variables use a macro of its own, `__dredd_thread_local`. A local `thread_local` variable of the mutated code then keeps the keyword, and MSVC no longer rejects it with C2480. | not opened |
| `upstream/adjacent-statements` | When a removed statement ends where the next statement starts (no white space after a `;`, a comment or the brace of a compound statement, as in `x = 1;y = 2;`), the closing brace of the removal goes before the text that opens the mutants of the next statement. The mutant build of such code used to nest the two in the wrong order, and MSVC stopped with C2143 and C2660. | not opened |
| `upstream/super-in-lazy-operand` (stacked on `upstream/tracking-prelude-without-stl`) | An operand that Dredd evaluates lazily, in a lambda, names the base class in place of a `__super` qualifier (`::Base::f()` for `__super::f()`): a lambda has no base class, so MSVC stopped with C2791. When the class cannot be named so, for example a dependent base, Dredd does not mutate that expression or operator. | not opened |
| `upstream/tracking-file-open-once` (stacked on `upstream/tracking-prelude-without-stl`) | Both tracking preludes open the tracking file once for each program, at the first reached mutation group, and keep the stream beside the lock of the file. Each group writes its lines and flushes them under the lock, so that a crash loses no recorded mutant. The preludes used to open and close the file for each group: on Windows, with the virus scanner, a test that reached thousands of groups took many times longer with the tracking than without it, above all with many test processes at the same time. | not opened |

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
old_prelude_without_stl=$(git rev-parse upstream/prelude-without-stl)
old_tracking_prelude_without_stl=$(git rev-parse upstream/tracking-prelude-without-stl)
git checkout main && git merge --ff-only upstream/main
for b in upstream/newer-llvm upstream/ms-compat-dependent-npc upstream/utf8-bom upstream/prelude-without-stl upstream/ignore-comments upstream/adjacent-statements; do
  git rebase main "$b"
done
git rebase --onto upstream/prelude-without-stl "$old_prelude_without_stl" upstream/tracking-prelude-without-stl
for b in upstream/prelude-thread-local-keyword upstream/super-in-lazy-operand upstream/tracking-file-open-once; do
  git rebase --onto upstream/tracking-prelude-without-stl "$old_tracking_prelude_without_stl" "$b"
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
- Mutated C files include `<stdatomic.h>`. Compile them with `/std:c11` (or
  later) and `/experimental:c11atomics`, or MSVC stops with C1189.
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
