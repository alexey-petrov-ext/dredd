import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

DREDD_REPO_ROOT = os.environ['DREDD_REPO_ROOT']
DREDD_EXECUTABLE = Path(DREDD_REPO_ROOT, 'temp', 'build-Debug', 'src', 'dredd', 'dredd') if 'DREDD_EXECUTABLE' not in os.environ else os.environ['DREDD_EXECUTABLE']


def run_dredd(source):
    info_file = source + '.json'
    result = subprocess.run([DREDD_EXECUTABLE, '--mutation-info-file', info_file, source, '--'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return result, info_file


def mutate(source):
    result, info_file = run_dredd(source)
    if result.returncode != 0:
        print(result.stderr.decode('utf-8'))
    assert result.returncode == 0, source
    with open(info_file) as file:
        return json.load(file)


def mutants(info):
    # Each mutant ID with its kind, its action and its first and last line.
    result = {}
    for info_for_file in info['infoForFiles']:
        for node in info_for_file['mutationTree']:
            for group in node['mutationGroups']:
                for kind, body in group.items():
                    start = body['start'] if 'start' in body else body['exprStart']
                    end = body['end'] if 'end' in body else body['exprEnd']
                    if kind == 'removeStmt':
                        entries = [(body['mutationId'], None)]
                    else:
                        entries = [(instance['mutationId'], instance['action']) for instance in body['instances']]
                    for mutation_id, action in entries:
                        result[mutation_id] = {'kind': kind, 'action': action, 'start': start['line'], 'end': end['line']}
    return result


def ignored(info):
    return {mutation['mutationId']: mutation for info_for_file in info['infoForFiles']
            for mutation in info_for_file.get('ignoredMutations', [])}


def check(source, expected, reason, comment_line):
    info = mutate(source)
    all_mutants = mutants(info)
    held = ignored(info)
    wanted = {mutation_id for mutation_id, mutant in all_mutants.items() if expected(mutant)}
    assert wanted, f'{source}: the case holds no mutant to ignore'
    assert set(held.keys()) == wanted, f'{source}: ignored {sorted(held.keys())}, expected {sorted(wanted)}'
    for mutation in held.values():
        assert mutation['reason'] == reason, f'{source}: reason {mutation["reason"]}'
        assert mutation['comment']['line'] == comment_line, f'{source}: comment {mutation["comment"]}'
    return all_mutants


def main():
    check('once_line.cc', lambda m: m['kind'] == 'replaceBinaryOperator' and m['start'] == 3,
          'The sum feeds a trace only.', 2)

    # The scope of once is the whole statement after the comment, also over 2 lines.
    all_mutants = check('once_statement.cc',
                        lambda m: m['kind'] == 'replaceBinaryOperator' and m['start'] >= 3 and m['end'] <= 4,
                        'The value goes to a trace.', 2)
    assert any(m['kind'] == 'replaceBinaryOperator' and m['end'] == 4 for m in all_mutants.values())

    # Before an if statement, once holds the statement with its body.
    all_mutants = check('once_if.cc', lambda m: m['start'] >= 4 and m['end'] <= 6, 'The branch only logs.', 3)
    assert any(m['kind'] == 'removeStmt' and m['start'] == 4 and m['end'] == 6 for m in all_mutants.values())

    check('once_function.cc', lambda m: m['start'] == 2, 'The function only formats a trace.', 1)

    # A block holds only the mutants that lie inside it, and not the removal of the loop around it.
    all_mutants = check('block.cc', lambda m: m['start'] >= 5 and m['end'] <= 5, 'The loop body only counts.', 4)
    assert any(m['kind'] == 'removeStmt' and m['start'] == 3 for m in all_mutants.values())

    check('no_restore.cc', lambda m: m['kind'] == 'replaceExpr' and m['start'] == 3,
          'The rest of the file only traces.', 2)

    check('actions.cc',
          lambda m: m['action'] in ('ReplaceWithLHS', 'ReplaceWithRHS') and m['start'] == 3,
          'Both operands are equal here.', 2)

    # A directive inside a string literal or a block comment is not a directive.
    info = mutate('not_directives.cc')
    assert mutants(info)
    assert not ignored(info)

    # A directive changes no mutant and no mutated code.
    for source in ('once_if.cc', 'block.cc'):
        copy = 'plain_' + source
        with open(source) as file:
            text = file.read()
        with open(copy, 'w') as file:
            file.write(text.replace('// Dredd ', '// Dredx '))
        shutil.copy(source, 'directive_' + source)
        with_directives = mutate('directive_' + source)
        without_directives = mutate(copy)
        assert mutants(with_directives) == mutants(without_directives), source
        with open('directive_' + source) as file:
            mutated_with = file.read()
        with open(copy) as file:
            mutated_without = file.read()
        assert mutated_with.replace('// Dredd ', '// Dredx ') == mutated_without, source

    # An unknown name stops Dredd on the file, names the location, and leaves the file unchanged.
    with open('unknown_name.cc') as file:
        original = file.read()
    result, _ = run_dredd('unknown_name.cc')
    assert result.returncode != 0
    error_output = result.stderr.decode('utf-8')
    assert 'unknown_name.cc:2:3' in error_output, error_output
    assert 'replaceExpression' in error_output, error_output
    with open('unknown_name.cc') as file:
        assert file.read() == original


if __name__ == '__main__':
    sys.exit(main())
