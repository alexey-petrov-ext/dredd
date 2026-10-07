import os
import subprocess
import sys
from pathlib import Path

DREDD_REPO_ROOT = os.environ['DREDD_REPO_ROOT']
DREDD_EXECUTABLE = Path(DREDD_REPO_ROOT, 'temp', 'build-Debug', 'src', 'dredd', 'dredd') if 'DREDD_EXECUTABLE' not in os.environ else os.environ['DREDD_EXECUTABLE']


def main():
    # -fms-compatibility is the default for MSVC targets. Dredd used to crash
    # when checking whether a value-dependent expression in a template is a
    # null pointer constant in this mode. -fno-delayed-template-parsing makes
    # Clang parse the template bodies, as it does by default on other targets.
    cmd = [DREDD_EXECUTABLE,
           'example.cc',
           '--',
           '-std=c++17',
           '-fms-compatibility',
           '-fno-delayed-template-parsing']
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr.decode('utf-8')
    mutated = open('example.cc').read()
    # Both templates must have been mutated.
    assert 'DREDD PRELUDE START' in mutated
    assert '__dredd_replace_binary_operator_Add' in mutated
    assert '__dredd_replace_binary_operator_Sub' in mutated


if __name__ == '__main__':
    sys.exit(main())
