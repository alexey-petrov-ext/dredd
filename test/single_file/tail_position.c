int Sign(int x) {
  if (x > 0) {
    return 1;
  } else if (x < 0) {
    return -1;
  } else {
    return 0;
  }
}

int Weight(int kind) {
  switch (kind) {
    case 0:
      return 10;
    default:
      return 1;
  }
}
