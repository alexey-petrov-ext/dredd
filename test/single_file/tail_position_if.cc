int calls;

int Sign(int x) {
  if (x > 0) {
    calls = calls + 1;
    return 1;
  } else if (x < 0) {
    return -1;
  } else {
    return 0;
  }
}

int AtLeastZero(int x) {
  if (x < 0) {
    return 0;
  }
  return x;
}

void Reset(int* x) {
  if (*x > 0) {
    *x = 0;
  } else {
    *x = 1;
  }
}
