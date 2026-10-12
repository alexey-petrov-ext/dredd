import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

DREDD_REPO_ROOT = os.environ['DREDD_REPO_ROOT']
DREDD_CLANG_LLVM_DIR = os.environ['DREDD_CLANG_LLVM_DIR']
DREDD_EXECUTABLE = Path(DREDD_REPO_ROOT, 'temp', 'build-Debug', 'src', 'dredd', 'dredd') if 'DREDD_EXECUTABLE' not in os.environ else os.environ['DREDD_EXECUTABLE']
QUERY_MUTANT_INFO_SCRIPT = Path(DREDD_REPO_ROOT, 'scripts', 'query_mutant_info.py')
CLANG_EXECUTABLE = Path(DREDD_CLANG_LLVM_DIR, 'bin', 'clang')
CLANGXX_EXECUTABLE = Path(DREDD_CLANG_LLVM_DIR, 'bin', 'clang++')
EXECUTABLE_SUFFIX = '.exe' if os.name == 'nt' else ''
THREAD_LINK_ARGS = [] if os.name == 'nt' else ['-pthread']

NUM_THREADS = 8
NUM_RUNS = 50
# A run that does not end in this time is taken to be blocked.
TIMEOUT_SECONDS = 120

# The prelude writes each mutant id with "%d\n".
MUTANT_ID_LINE = re.compile(r'0|[1-9][0-9]*')


def run_successfully(cmd):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print(f"Command exited with error code {result.returncode}: {' '.join(str(part) for part in cmd)}")
        print(result.stdout.decode('utf-8'))
        print(result.stderr.decode('utf-8'))
        sys.exit(1)
    return result


def run_tracked(executable, num_threads, tracking_file):
    dredd_env = os.environ.copy()
    dredd_env.pop('DREDD_ENABLED_MUTATION', None)
    dredd_env.pop('DREDD_MUTANT_TRACKING_FILE', None)
    if tracking_file is not None:
        dredd_env['DREDD_MUTANT_TRACKING_FILE'] = str(tracking_file)
    result = subprocess.run([executable, str(num_threads)], env=dredd_env, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=TIMEOUT_SECONDS)
    assert result.returncode == 0, f"{executable} {num_threads} with DREDD_MUTANT_TRACKING_FILE={tracking_file!s} gave exit code {result.returncode}"


def tracked_lines(executable, num_threads, tracking_file):
    if tracking_file.exists():
        tracking_file.unlink()
    run_tracked(executable, num_threads, tracking_file)
    return tracking_file.read_text().splitlines()


def problems(lines, expected):
    # Lists each way in which the lines of a tracking file differ from naming
    # each expected mutant once.
    malformed = [line for line in lines if not MUTANT_ID_LINE.fullmatch(line)]
    counts = Counter(int(line) for line in lines if MUTANT_ID_LINE.fullmatch(line))
    missing = sorted(expected - counts.keys())
    unexpected = sorted(counts.keys() - expected)
    repeated = sorted(mutant for mutant, count in counts.items() if count > 1)
    result = []
    if malformed:
        result.append(f"{len(malformed)} malformed lines, such as {malformed[:3]}")
    if missing:
        result.append(f"{len(missing)} missing mutants, such as {missing[:5]}")
    if unexpected:
        result.append(f"{len(unexpected)} unexpected mutants, such as {unexpected[:5]}")
    if repeated:
        result.append(f"{len(repeated)} mutants named more than once, such as {repeated[:5]}")
    return result


def check_tracking(executable, all_mutants):
    tracking_file = Path(Path(executable).stem + '.txt')

    # Every call reaches all mutants of the function that it calls, so a run in
    # one thread names every mutant of both files, once.
    expected_lines = tracked_lines(executable, 1, tracking_file)
    assert not problems(expected_lines, all_mutants), problems(expected_lines, all_mutants)
    expected = set(int(line) for line in expected_lines)

    # When several threads reach the mutants at the same time, the file must
    # name the same mutants, each once, with no damaged line.
    failures = []
    for run in range(NUM_RUNS):
        found = problems(tracked_lines(executable, NUM_THREADS, tracking_file), expected)
        if found:
            failures.append(f"run {run + 1}: " + "; ".join(found))
    assert not failures, (f"{executable}: {len(failures)} of {NUM_RUNS} runs with {NUM_THREADS} threads wrote a wrong "
                          f"tracking file:\n" + "\n".join(failures))

    # Without a tracking file, and with one that cannot be opened, the program
    # runs as the original program does.
    run_tracked(executable, NUM_THREADS, None)
    run_tracked(executable, NUM_THREADS, Path('no_such_directory', 'covered.txt'))


def main():
    # One invocation of Dredd gives the mutants of both files distinct ids.
    shutil.copyfile(src='cxx_work.cc', dst='cxx_mutated.cc')
    shutil.copyfile(src='c_work.c', dst='c_mutated.c')
    run_successfully([DREDD_EXECUTABLE, '--only-track-mutant-coverage', '--mutation-info-file', 'info.json',
                      'cxx_mutated.cc', 'c_mutated.c', '--'])
    largest_mutant_id = int(run_successfully(
        [sys.executable, QUERY_MUTANT_INFO_SCRIPT, '--largest-mutant-id', 'info.json']).stdout.decode('utf-8'))
    all_mutants = set(range(0, largest_mutant_id + 1))

    # The program holds a mutated C++ file and a mutated C file, which must
    # share one lock for the tracking file.
    run_successfully([CLANGXX_EXECUTABLE, '-c', 'cxx_mutated.cc', '-o', 'cxx_mutated.o'])
    run_successfully([CLANG_EXECUTABLE, '-c', 'c_mutated.c', '-o', 'c_mutated.o'])
    clang_executable = str(Path.cwd() / ('clang' + EXECUTABLE_SUFFIX))
    run_successfully([CLANGXX_EXECUTABLE, 'harness.cc', 'cxx_mutated.o', 'c_mutated.o', '-o', clang_executable]
                     + THREAD_LINK_ARGS)
    check_tracking(clang_executable, all_mutants)

    # The same with the MSVC compiler. Mutated C files include <stdatomic.h>,
    # which MSVC provides only for C11 or later, with /experimental:c11atomics.
    if os.name == 'nt' and shutil.which('cl') is not None:
        msvc_executable = str(Path.cwd() / 'msvc.exe')
        run_successfully(['cl', '/nologo', '/c', '/std:c11', '/experimental:c11atomics', 'c_mutated.c',
                          '/Foc_mutated.obj'])
        run_successfully(['cl', '/nologo', '/EHsc', 'cxx_mutated.cc', 'harness.cc', 'c_mutated.obj',
                          f'/Fe{msvc_executable}'])
        check_tracking(msvc_executable, all_mutants)


if __name__ == '__main__':
    sys.exit(main())
