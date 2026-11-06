extern "C" int abs(int);
extern "C" double fabs(double);

namespace geometry {
int abs(int x);
}  // namespace geometry

int distance;
double magnitude;

void Measure(int a, int b, double x) {
  distance = abs(a - b) + geometry::abs(a);
  magnitude = -fabs(x);
}
