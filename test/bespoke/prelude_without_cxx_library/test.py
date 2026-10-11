import os
import subprocess
import sys
from pathlib import Path

DREDD_REPO_ROOT = os.environ['DREDD_REPO_ROOT']
DREDD_CLANG_LLVM_DIR = os.environ['DREDD_CLANG_LLVM_DIR']
DREDD_EXECUTABLE = Path(DREDD_REPO_ROOT, 'temp', 'build-Debug', 'src', 'dredd', 'dredd') if 'DREDD_EXECUTABLE' not in os.environ else os.environ['DREDD_EXECUTABLE']
QUERY_MUTANT_INFO_SCRIPT = Path(DREDD_REPO_ROOT, 'scripts', 'query_mutant_info.py')
CLANG_EXECUTABLE = Path(DREDD_CLANG_LLVM_DIR, 'bin', 'clang')
CLANGXX_EXECUTABLE = Path(DREDD_CLANG_LLVM_DIR, 'bin', 'clang++')
COMPILED_EXECUTABLE_FILENAME = str(Path.cwd() / ('a.exe' if os.name == 'nt' else 'a.out'))

# The C compiler driver links no C++ standard library. On Windows, the headers
# of the Microsoft C++ standard library name that library for the linker, so
# the link also excludes it.
NO_CXX_LIBRARY_LINK_ARGS = ['-Xlinker', '/NODEFAULTLIB:libcpmt.lib', '-Xlinker', '/NODEFAULTLIB:msvcprt.lib'] if os.name == 'nt' else []


def run_successfully(cmd):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print(f"Command exited with error code {result.returncode}: {' '.join(str(part) for part in cmd)}")
        print(result.stdout.decode('utf-8'))
        print(result.stderr.decode('utf-8'))
        sys.exit(1)
    return result


def run_program(enabled_mutation):
    dredd_env = os.environ.copy()
    dredd_env.pop('DREDD_ENABLED_MUTATION', None)
    if enabled_mutation is not None:
        dredd_env['DREDD_ENABLED_MUTATION'] = enabled_mutation
    return subprocess.run([COMPILED_EXECUTABLE_FILENAME], env=dredd_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def output_of(enabled_mutation):
    result = run_program(enabled_mutation)
    assert result.returncode == 0, f"DREDD_ENABLED_MUTATION={enabled_mutation!r} gave exit code {result.returncode}"
    return result.stdout.decode('utf-8').strip()


def main():
    run_successfully([DREDD_EXECUTABLE, '--mutation-info-file', 'info.json', 'example.cc', '--'])

    # The mutated code must need no function of the C++ standard library.
    run_successfully([CLANGXX_EXECUTABLE, '-c', 'example.cc', '-o', 'example.o'])
    run_successfully([CLANGXX_EXECUTABLE, '-c', 'harness.cc', '-o', 'harness.o'])
    run_successfully([CLANG_EXECUTABLE, 'example.o', 'harness.o', '-o', COMPILED_EXECUTABLE_FILENAME]
                     + NO_CXX_LIBRARY_LINK_ARGS)

    largest_mutant_id = int(run_successfully(
        ["python", QUERY_MUTANT_INFO_SCRIPT, "--largest-mutant-id", "info.json"]).stdout.decode('utf-8'))

    original = output_of(None)
    assert original == '17'
    assert output_of('') == original
    # An id of no mutant of this file enables nothing.
    assert output_of(str(largest_mutant_id + 1)) == original

    mutant = next(mutant_id for mutant_id in range(largest_mutant_id + 1)
                  if run_program(str(mutant_id)).returncode == 0 and output_of(str(mutant_id)) != original)
    mutated = output_of(str(mutant))

    # The list skips empty tokens and the ids of mutants of other files.
    assert output_of(f'{mutant},') == mutated
    assert output_of(f',{mutant}') == mutated
    assert output_of(f'{largest_mutant_id + 1},{mutant}') == mutated

    # A token with no number stops the program.
    assert run_program(f'x,{mutant}').returncode != 0


if __name__ == '__main__':
    sys.exit(main())
