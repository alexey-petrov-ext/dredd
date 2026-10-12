static int calls = 0;

static int Next() { return ++calls; }

// The harness calls this function, and each of its statements and operands is
// evaluated, so the run reaches every mutant of the function. Dredd passes the
// right operand of && and an operand with a side effect to its mutator
// functions as a lambda.
int Reached(int x, int y) {
  int result = x * 3 + y;
  if (x > 0 && y > 0) {
    result += Next();
  }
  return result;
}

// The harness never calls this function, so the run reaches none of its
// mutants.
int NotReached(int x, int y) { return x - y; }
