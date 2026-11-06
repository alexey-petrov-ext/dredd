int Weight(int kind) {
  switch (kind) {
    case 0:
      return 10;
    case 1:
    case 2:
      kind = kind * 2;
      return kind;
    default:
      return 1;
  }
}

int Code(int kind) {
  switch (kind) {
    case 0:
      return 5;
    default:;
  }
}

int Half(int x) {
  if (x < 0) {
    goto negative;
  }
  return x / 2;
negative:
  x = 0;
  return x;
}
