void Assign(int& x) {
  x = int{2}
#if 1
  ;
#endif
}

void AssignInIf(int& x, bool c) {
  if (c) x = int{2}
#if 1
  ;
#endif
}
