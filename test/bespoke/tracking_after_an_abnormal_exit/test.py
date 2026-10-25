import os
import shutil
import subprocess
import sys
from pathlib import Path

DREDD_REPO_ROOT = os.environ['DREDD_REPO_ROOT']
DREDD_CLANG_LLVM_DIR = os.environ['DREDD_CLANG_LLVM_DIR']
DREDD_EXECUTABLE = Path(DREDD_REPO_ROOT, 'temp', 'build-Debug', 'src', 'dredd', 'dredd') if 'DREDD_EXECUTABLE' not in os.environ else os.environ['DREDD_EXECUTABLE']
QUERY_MUTANT_INFO_SCRIPT = Path(DREDD_REPO_ROOT, 'scripts', 'query_mutant_info.py')
CLANG_EXECUTABLE = Path(DREDD_CLANG_LLVM_DIR, 'bin', 'clang')
CLANGXX_EXECUTABLE = Path(DREDD_CLANG_LLVM_DIR, 'bin', 'clang++')
EXECUTABLE_SUFFIX = '.exe' if os.name == 'nt' else ''

# The harness ends with this exit code, and without flushing a stream.
ABNORMAL_EXIT_CODE = 3


def run_successfully(cmd):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print(f"Command exited with error code {result.returncode}: {' '.join(str(part) for part in cmd)}")
        print(result.stdout.decode('utf-8'))
        print(result.stderr.decode('utf-8'))
        sys.exit(1)
    return result


def check_tracking(executable, all_mutants):
    # The tracking file stays open while the program runs, and the program ends
    # without closing it. The file must still name each mutant that the program
    # reached, once.
    tracking_file = Path(Path(executable).stem + '.txt')
    if tracking_file.exists():
        tracking_file.unlink()
    dredd_env = os.environ.copy()
    dredd_env.pop('DREDD_ENABLED_MUTATION', None)
    dredd_env['DREDD_MUTANT_TRACKING_FILE'] = str(tracking_file)
    result = subprocess.run([executable], env=dredd_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == ABNORMAL_EXIT_CODE, f"{executable} gave exit code {result.returncode}"
    recorded = sorted(int(line) for line in tracking_file.read_text().split())
    assert recorded == sorted(all_mutants), f"{executable} recorded {recorded}, and not {sorted(all_mutants)}"


def main():
    # One invocation of Dredd gives the mutants of both files distinct ids.
    shutil.copyfile(src='cxx_work.cc', dst='cxx_mutated.cc')
    shutil.copyfile(src='c_work.c', dst='c_mutated.c')
    run_successfully([DREDD_EXECUTABLE, '--only-track-mutant-coverage', '--mutation-info-file', 'info.json',
                      'cxx_mutated.cc', 'c_mutated.c', '--'])
    largest_mutant_id = int(run_successfully(
        [sys.executable, QUERY_MUTANT_INFO_SCRIPT, '--largest-mutant-id', 'info.json']).stdout.decode('utf-8'))
    all_mutants = set(range(0, largest_mutant_id + 1))

    run_successfully([CLANGXX_EXECUTABLE, '-c', 'cxx_mutated.cc', '-o', 'cxx_mutated.o'])
    run_successfully([CLANG_EXECUTABLE, '-c', 'c_mutated.c', '-o', 'c_mutated.o'])
    clang_executable = str(Path.cwd() / ('clang' + EXECUTABLE_SUFFIX))
    run_successfully([CLANGXX_EXECUTABLE, 'harness.cc', 'cxx_mutated.o', 'c_mutated.o', '-o', clang_executable])
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
