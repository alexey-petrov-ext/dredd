#!/usr/bin/env python3

# Copyright 2026 The Dredd Project Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Cross-platform runner for Dredd's single-file and execute tests.

Unlike the bash scripts in this directory, this runner does not need a POSIX
shell, so it also works with MSVC (cl.exe) on Windows. For every test it
checks that:

  1. Dredd succeeds and writes valid mutation info (JSON);
  2. the mutated file matches the golden '.expected' file (single-file tests);
  3. the mutated file compiles with the chosen compiler and language standard;
  4. the mutated program behaves as expected, with and without mutants
     enabled (execute tests).

Known, platform-specific deviations are listed in an expectations file (see
--expectations). A result that matches an expectation is reported as XFAIL and
does not fail the run; a listed expectation that no longer happens is reported
as XPASS so that the list can be pruned.

Exit code: 0 if there are no unexpected failures, 1 otherwise.
"""

import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

REPO_ROOT = Path(__file__).resolve().parent.parent

# Result categories, in the order in which a test can fail.
DREDD_FAIL = 'dredd-fail'
GOLDEN_DIFF = 'golden-diff'
COMPILE_FAIL = 'compile-fail'
RUN_FAIL = 'run-fail'
CATEGORIES = [DREDD_FAIL, GOLDEN_DIFF, COMPILE_FAIL, RUN_FAIL]


@dataclass
class Result:
    test: str  # Repository-relative path, with forward slashes.
    variant: str  # 'opt', 'noopt' or 'execute'.
    failures: List[Tuple[str, str]] = field(default_factory=list)

    def add(self, category: str, details: str) -> None:
        self.failures.append((category, details))


class Compiler:
    """Builds compiler command lines for GNU-style and MSVC-style drivers."""

    def __init__(self, cc: str, cxx: str, style: str, c_std: str,
                 cxx_std: str, extra_args: List[str]):
        self.cc = cc
        self.cxx = cxx
        self.style = style
        self.c_std = c_std
        self.cxx_std = cxx_std
        self.extra_args = extra_args

    def _base(self, is_cxx: bool) -> List[str]:
        if self.style == 'msvc':
            cmd = [self.cxx if is_cxx else self.cc, '/nologo']
            if is_cxx:
                cmd += ['/TP', '/EHsc', f'/std:{self.cxx_std}']
            else:
                # Dredd's C prelude includes <stdatomic.h>, which MSVC
                # supports only with this option.
                cmd += ['/TC', f'/std:{self.c_std}',
                        '/experimental:c11atomics']
        else:
            cmd = [self.cxx if is_cxx else self.cc,
                   f'-std={self.cxx_std if is_cxx else self.c_std}']
        return cmd + self.extra_args

    def compile_only(self, source: Path, is_cxx: bool) -> List[str]:
        if self.style == 'msvc':
            return self._base(is_cxx) + ['/c', str(source),
                                         f'/Fo{source.with_suffix(".obj")}']
        return self._base(is_cxx) + ['-c', str(source), '-o',
                                     str(source.with_suffix('.o'))]

    def compile_and_link(self, sources: List[Path], exe: Path,
                         is_cxx: bool) -> List[str]:
        if self.style == 'msvc':
            return (self._base(is_cxx) + [str(s) for s in sources] +
                    [f'/Fe{exe}', f'/Fo{exe.parent}{os.sep}'])
        return self._base(is_cxx) + [str(s) for s in sources] + ['-o', str(exe)]

    def dredd_std_args(self, is_cxx: bool) -> List[str]:
        # Ask Dredd to parse with the same language standard that the mutated
        # code will be compiled with.
        return [f'-std={self.cxx_std if is_cxx else self.c_std}']


def run(cmd: List[str], cwd: Path, env: Optional[Dict[str, str]] = None,
        timeout: int = 600) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout,
                          check=False)


def output_of(proc: subprocess.CompletedProcess, limit: int = 4000) -> str:
    text = proc.stdout.decode('utf-8', errors='replace')
    return text if len(text) <= limit else text[:limit] + '\n[...]'


def read_text(path: Path) -> str:
    # Compare as text with CRLF normalised, like 'diff --strip-trailing-cr'.
    return path.read_bytes().decode('utf-8', errors='replace').replace(
        '\r\n', '\n')


def run_dredd(dredd: str, work: Path, filename: str, extra: List[str],
              clang_args: List[str], result: Result) -> bool:
    cmd = [dredd] + extra + ['--mutation-info-file', 'temp.json', filename,
                             '--'] + clang_args
    proc = run(cmd, work)
    if proc.returncode != 0:
        result.add(DREDD_FAIL, f'exit code {proc.returncode}\n' +
                   output_of(proc))
        return False
    try:
        with open(work / 'temp.json', encoding='utf-8') as json_file:
            json.load(json_file)
    except (OSError, ValueError) as error:
        result.add(DREDD_FAIL, f'invalid mutation info JSON: {error}')
        return False
    return True


def single_file_test(test: Path, variant: str, args: argparse.Namespace,
                     compiler: Compiler, scratch: Path) -> Result:
    rel = test.relative_to(REPO_ROOT).as_posix()
    result = Result(rel, variant)
    work = scratch / f'{test.name}.{variant}'
    work.mkdir(parents=True)
    shutil.copy(test, work / test.name)
    is_cxx = test.suffix == '.cc'
    extra = ['--no-mutation-opts'] if variant == 'noopt' else []
    clang_args = (compiler.dredd_std_args(is_cxx) if args.dredd_std else
                  []) + args.dredd_arg
    if not run_dredd(args.dredd, work, test.name, extra, clang_args, result):
        return result

    if args.golden != 'off':
        expected = test.parent / (test.name + ('.noopt.expected'
                                               if variant == 'noopt'
                                               else '.expected'))
        if not expected.exists():
            result.add(GOLDEN_DIFF, f'missing {expected.name}')
        elif read_text(work / test.name) != read_text(expected):
            result.add(GOLDEN_DIFF, f'output differs from {expected.name}')

    proc = run(compiler.compile_only(work / test.name, is_cxx), work)
    if proc.returncode != 0:
        result.add(COMPILE_FAIL, output_of(proc))
    return result


def execute_test(test_dir: Path, args: argparse.Namespace, compiler: Compiler,
                 scratch: Path) -> Result:
    rel = test_dir.relative_to(REPO_ROOT).as_posix()
    result = Result(rel, 'execute')
    work = scratch / f'{test_dir.name}.execute'
    work.mkdir(parents=True)
    is_cxx = (test_dir / 'harness.cc').exists()
    ext = 'cc' if is_cxx else 'c'
    for name in [f'harness.{ext}', f'tomutate.{ext}']:
        shutil.copy(test_dir / name, work / name)

    expected_original = (test_dir / 'original.txt').read_text().strip()
    expected_mutant_outputs: Set[str] = set()
    for line in (test_dir / 'mutants.txt').read_text().splitlines():
        component = line.split(';')[0].strip()
        if component:
            expected_mutant_outputs.add(component)

    clang_args = compiler.dredd_std_args(is_cxx) + args.dredd_arg
    if not run_dredd(args.dredd, work, f'tomutate.{ext}', [], clang_args,
                     result):
        return result

    exe = work / ('test_executable.exe' if os.name == 'nt' else
                  'test_executable')
    proc = run(compiler.compile_and_link(
        [work / f'harness.{ext}', work / f'tomutate.{ext}'], exe, is_cxx),
        work)
    if proc.returncode != 0:
        result.add(COMPILE_FAIL, output_of(proc))
        return result

    env = dict(os.environ)
    env.pop('DREDD_ENABLED_MUTATION', None)
    proc = run([str(exe)], work, env=env, timeout=60)
    actual = proc.stdout.decode('utf-8', errors='replace').strip()
    if proc.returncode != 0 or actual != expected_original:
        result.add(RUN_FAIL, f'non-mutated run: exit {proc.returncode}, '
                   f'expected {expected_original!r}, got {actual!r}')
        return result

    query = run([sys.executable,
                 str(REPO_ROOT / 'scripts' / 'query_mutant_info.py'),
                 '--largest-mutant-id', 'temp.json'], work)
    if query.returncode != 0:
        result.add(RUN_FAIL, 'query_mutant_info.py failed\n' +
                   output_of(query))
        return result
    largest_mutant_id = int(query.stdout.decode('utf-8').strip())

    mutant_id = 0
    while mutant_id <= largest_mutant_id and expected_mutant_outputs:
        env['DREDD_ENABLED_MUTATION'] = str(mutant_id)
        try:
            proc = run([str(exe)], work, env=env, timeout=60)
        except subprocess.TimeoutExpired:
            mutant_id += 1
            continue
        if proc.returncode == 0:
            expected_mutant_outputs.discard(
                proc.stdout.decode('utf-8', errors='replace').strip())
        mutant_id += 1
    if expected_mutant_outputs:
        result.add(RUN_FAIL, 'unmatched expected mutant outputs: ' +
                   ', '.join(sorted(expected_mutant_outputs)))
    return result


def load_expectations(path: Optional[Path]) -> Set[Tuple[str, str, str]]:
    """Reads lines of the form '<category> <test> [<variant>]  # reason'."""
    expectations: Set[Tuple[str, str, str]] = set()
    if path is None or not path.exists():
        return expectations
    for number, line in enumerate(path.read_text().splitlines(), start=1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if parts[0] not in CATEGORIES or len(parts) not in (2, 3):
            sys.exit(f'{path}:{number}: expected "<category> <test> '
                     f'[<variant>]", with category one of {CATEGORIES}')
        variant = parts[2] if len(parts) == 3 else '*'
        expectations.add((parts[0], parts[1], variant))
    return expectations


def is_expected(expectations: Set[Tuple[str, str, str]], category: str,
                result: Result) -> Optional[Tuple[str, str, str]]:
    for variant in (result.variant, '*'):
        key = (category, result.test, variant)
        if key in expectations:
            return key
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--dredd', required=True,
                        help='Path to the dredd executable.')
    parser.add_argument('--style', choices=['gnu', 'msvc'], default='gnu',
                        help='Command-line style of the compiler.')
    parser.add_argument('--cc', default=None, help='C compiler.')
    parser.add_argument('--cxx', default=None, help='C++ compiler.')
    parser.add_argument('--c-std', default='c17')
    parser.add_argument('--cxx-std', default='c++17')
    parser.add_argument('--extra-compiler-arg', action='append', default=[],
                        help='Extra argument for every compile (repeatable).')
    parser.add_argument('--dredd-arg', action='append', default=[],
                        help='Extra Clang argument for Dredd, passed after '
                        '"--" (repeatable).')
    parser.add_argument('--suite', action='append',
                        choices=['single_file', 'execute'],
                        help='Test suites to run (default: both).')
    parser.add_argument('--filter', default=None,
                        help='Only run tests whose path contains this text.')
    parser.add_argument('--golden', choices=['check', 'off'], default='check',
                        help='Compare single-file output with golden files.')
    parser.add_argument('--dredd-std', action='store_true',
                        help='Also pass --c-std/--cxx-std to Dredd for '
                        'single-file tests (execute tests always do).')
    parser.add_argument('--expectations', type=Path, default=None,
                        help='File listing known deviations (XFAIL).')
    parser.add_argument('--suggest', type=Path, default=None,
                        help='Write an expectations file covering all '
                        'failures of this run to this path.')
    parser.add_argument('--summary', type=Path, default=None,
                        help='Append a Markdown summary to this file, e.g. '
                        '$GITHUB_STEP_SUMMARY.')
    parser.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 2)
    parser.add_argument('--keep', action='store_true',
                        help='Keep the scratch directory.')
    args = parser.parse_args()

    default_cc, default_cxx = (('cl', 'cl') if args.style == 'msvc' else
                               (os.environ.get('CC', 'cc'),
                                os.environ.get('CXX', 'c++')))
    compiler = Compiler(args.cc or default_cc, args.cxx or default_cxx,
                        args.style, args.c_std, args.cxx_std,
                        args.extra_compiler_arg)
    args.dredd = str(Path(args.dredd).resolve())
    suites = args.suite or ['single_file', 'execute']
    expectations = load_expectations(args.expectations)

    scratch = Path(tempfile.mkdtemp(prefix='dredd-tests-'))
    jobs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        if 'single_file' in suites:
            tests = sorted(list((REPO_ROOT / 'test' / 'single_file').glob(
                '*.cc')) + list((REPO_ROOT / 'test' / 'single_file').glob(
                    '*.c')))
            for test in tests:
                if args.filter and args.filter not in test.as_posix():
                    continue
                for variant in ('opt', 'noopt'):
                    jobs.append(pool.submit(single_file_test, test, variant,
                                            args, compiler, scratch))
        if 'execute' in suites:
            for test_dir in sorted((REPO_ROOT / 'test' / 'execute').iterdir()):
                if not test_dir.is_dir():
                    continue
                if args.filter and args.filter not in test_dir.as_posix():
                    continue
                jobs.append(pool.submit(execute_test, test_dir, args,
                                        compiler, scratch))
        results = [job.result() for job in jobs]

    unexpected: List[Tuple[Result, str, str]] = []
    xfail: List[Tuple[Result, str]] = []
    matched: Set[Tuple[str, str, str]] = set()
    for result in sorted(results, key=lambda r: (r.test, r.variant)):
        for category, details in result.failures:
            key = is_expected(expectations, category, result)
            if key:
                matched.add(key)
                xfail.append((result, category))
            else:
                unexpected.append((result, category, details))
    ran = {(r.test, r.variant) for r in results}
    xpass = sorted(e for e in expectations - matched
                   if any(t == e[1] and (e[2] in ('*', v)) for t, v in ran))

    for result, category, details in unexpected:
        print(f'FAIL   [{category}] {result.test} ({result.variant})')
        for line in details.splitlines():
            print(f'       {line}')
    for result, category in xfail:
        print(f'XFAIL  [{category}] {result.test} ({result.variant})')
    for category, test, variant in xpass:
        print(f'XPASS  [{category}] {test} ({variant}): remove it from the '
              'expectations file')

    passed = sum(1 for r in results if not r.failures)
    by_category = {c: sum(1 for _, cat, _ in unexpected if cat == c)
                   for c in CATEGORIES}
    headline = (f'{len(results)} test runs: {passed} passed, '
                f'{len(unexpected)} unexpected failures, {len(xfail)} '
                f'expected failures, {len(xpass)} unexpected passes')
    print(headline)

    if args.suggest is not None:
        lines = sorted({f'{c} {r.test} {r.variant}'
                        for r in results for c, _ in r.failures})
        args.suggest.write_text(
            '# Generated by check_tests_with_compiler.py; review before use.\n'
            + '\n'.join(lines) + ('\n' if lines else ''))

    if args.summary is not None:
        with open(args.summary, 'a', encoding='utf-8') as summary:
            summary.write(f'### Dredd tests ({compiler.style}, '
                          f'{compiler.cxx_std} / {compiler.c_std})\n\n'
                          f'{headline}\n\n')
            summary.write('| Category | Unexpected |\n|---|---|\n')
            for category in CATEGORIES:
                summary.write(f'| {category} | {by_category[category]} |\n')
            if unexpected:
                summary.write('\n<details><summary>Unexpected failures'
                              '</summary>\n\n```\n')
                for result, category, _ in unexpected:
                    summary.write(f'{category} {result.test} '
                                  f'{result.variant}\n')
                summary.write('```\n</details>\n')
            summary.write('\n')

    if args.keep:
        print(f'Scratch directory kept: {scratch}')
    else:
        shutil.rmtree(scratch, ignore_errors=True)
    return 1 if unexpected else 0


if __name__ == '__main__':
    sys.exit(main())
