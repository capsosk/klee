// RUN: %clang -emit-llvm -c -o %t1.bc %s
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --exit-on-error %t1.bc

#include <stdio.h>

#define ADDRESS ((int*) 0x0080)

int main() {
  klee_define_fixed_object(ADDRESS, 8);
  
  int *p = ADDRESS;

  p[1] = 10;
  printf("*p: %d\n", *p);
	printf("p[1]: %d\n", p[1]);

  return 0;
}
