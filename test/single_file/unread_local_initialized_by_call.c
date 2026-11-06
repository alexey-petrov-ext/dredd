int Write(int* file);

int Count(void);

void Call(void) {
  int extent = Count();
}

void CallsInBothOperands(void) {
  int total = Count() + Count();
}

void ShortCircuit(int* file) {
  int both = Write(file) && Write(file);
}
