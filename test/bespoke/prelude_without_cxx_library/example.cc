static int calls = 0;

static int Next() { return ++calls; }

int Scale(int x, int y) { return x * 3 + y; }

// Dredd passes the right operand of && and ||, and an operand with a side
// effect, to its mutator functions as a lambda.
int Choose(int x, int y) {
  if (x > 0 && y > 0) {
    return Next() + x;
  }
  return x < 0 || y < 0 ? -1 : 0;
}
