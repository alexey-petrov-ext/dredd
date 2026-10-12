import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

DREDD_REPO_ROOT = os.environ['DREDD_REPO_ROOT']
DREDD_CLANG_LLVM_DIR = os.environ['DREDD_CLANG_LLVM_DIR']
DREDD_EXECUTABLE = Path(DREDD_REPO_ROOT, 'temp', 'build-Debug', 'src', 'dredd', 'dredd') if 'DREDD_EXECUTABLE' not in os.environ else os.environ['DREDD_EXECUTABLE']
CLANG_EXECUTABLE = Path(DREDD_CLANG_LLVM_DIR, 'bin', 'clang')
CLANGXX_EXECUTABLE = Path(DREDD_CLANG_LLVM_DIR, 'bin', 'clang++')
EXECUTABLE_SUFFIX = '.exe' if os.name == 'nt' else ''

# The C compiler driver links no C++ standard library. On Windows, the headers
# of the Microsoft C++ standard library name that library for the linker, so
# the link also excludes it.
NO_CXX_LIBRARY_LINK_ARGS = ['-Xlinker', '/NODEFAULTLIB:libcpmt.lib', '-Xlinker', '/NODEFAULTLIB:msvcprt.lib'] if os.name == 'nt' else []

EXPECTED_OUTPUT = ['18', '19']


def run_successfully(cmd):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        print(f"Command exited with error code {result.returncode}: {' '.join(str(part) for part in cmd)}")
        print(result.stdout.decode('utf-8'))
        print(result.stderr.decode('utf-8'))
        sys.exit(1)
    return result


def mutant_lines(info_filename):
    # Maps each mutant id to the line at which its mutation starts.
    with open(info_filename, 'r') as info_file:
        info = json.load(info_file)
    result = {}
    for file_info in info['infoForFiles']:
        for mutation_tree_node in file_info['mutationTree']:
            for mutation_group in mutation_tree_node['mutationGroups']:
                (kind, mutation), = mutation_group.items()
                if kind == 'removeStmt':
                    result[mutation['mutationId']] = mutation['start']['line']
                    continue
                start = mutation['start'] if kind == 'replaceExpr' else mutation['exprStart']
                for instance in mutation['instances']:
                    result[instance['mutationId']] = start['line']
    return result


def run_tracked(executable, tracking_file):
    dredd_env = os.environ.copy()
    dredd_env.pop('DREDD_ENABLED_MUTATION', None)
    dredd_env.pop('DREDD_MUTANT_TRACKING_FILE', None)
    if tracking_file is not None:
        dredd_env['DREDD_MUTANT_TRACKING_FILE'] = tracking_file
    result = subprocess.run([executable], env=dredd_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, f"{executable} with DREDD_MUTANT_TRACKING_FILE={tracking_file!r} gave exit code {result.returncode}"
    assert result.stdout.decode('utf-8').split() == EXPECTED_OUTPUT


def recorded_mutants(tracking_file):
    return sorted(int(line) for line in Path(tracking_file).read_text().split())


def check_tracking(executable, reached):
    # Without a tracking file, and with one that cannot be opened, the program
    # runs as the original program does.
    run_tracked(executable, None)
    run_tracked(executable, str(Path('no_such_directory', 'covered.txt')))

    # The file names each mutant that the run reaches, once, and no other
    # mutant.
    tracking_file = Path(executable).stem + '.txt'
    run_tracked(executable, tracking_file)
    assert recorded_mutants(tracking_file) == sorted(reached)

    # A second run appends to the file.
    run_tracked(executable, tracking_file)
    assert recorded_mutants(tracking_file) == sorted(reached * 2)


def main():
    shutil.copyfile(src='example.cc', dst='tomutate.cc')
    run_successfully([DREDD_EXECUTABLE, '--only-track-mutant-coverage', '--mutation-info-file', 'info.json',
                      'tomutate.cc', '--'])

    # The harness calls Reached and not NotReached, which follows it.
    not_reached_line = next(number for number, line in enumerate(Path('example.cc').read_text().splitlines(), 1)
                            if line.startswith('int NotReached('))
    lines = mutant_lines('info.json')
    reached = [mutant for mutant, line in lines.items() if line < not_reached_line]
    assert len(reached) > 0
    assert len(reached) < len(lines)

    # The mutated code must need no function of the C++ standard library.
    run_successfully([CLANGXX_EXECUTABLE, '-c', 'tomutate.cc', '-o', 'tomutate.o'])
    run_successfully([CLANGXX_EXECUTABLE, '-c', 'harness.cc', '-o', 'harness.o'])
    clang_executable = str(Path.cwd() / ('clang' + EXECUTABLE_SUFFIX))
    run_successfully([CLANG_EXECUTABLE, 'tomutate.o', 'harness.o', '-o', clang_executable]
                     + NO_CXX_LIBRARY_LINK_ARGS)
    check_tracking(clang_executable, reached)

    # With MSVC, the mutated code must also link when it is compiled for the
    # debug runtime and linked with the release C runtime, which provides no
    # function of the debug runtime.
    if os.name == 'nt' and shutil.which('cl') is not None:
        msvc_executable = str(Path.cwd() / 'msvc.exe')
        run_successfully(['cl', '/nologo', '/MTd', '/D_DEBUG', '/EHsc', 'tomutate.cc', 'harness.cc',
                          f'/Fe{msvc_executable}', '/link', '/NODEFAULTLIB:libcmtd.lib', 'libcmt.lib'])
        check_tracking(msvc_executable, reached)


if __name__ == '__main__':
    sys.exit(main())
