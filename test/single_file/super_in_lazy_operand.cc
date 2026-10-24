class Base {
 public:
  bool Ok();
};

// __super exists only with Microsoft extensions.
#ifdef _MSC_VER
class Derived : public Base {
 public:
  bool Initializer() {
    bool valid = __super::Ok();
    return valid;
  }

  bool Operand(bool flag) { return flag && __super::Ok(); }

  bool Return() { return __super::Ok(); }
};
#endif
