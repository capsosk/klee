// RUN: %clang %s -g -emit-llvm %O0opt -c -o %t1.bc
// RUN: rm -rf %t.klee-out
// RUN: %klee --output-dir=%t.klee-out --emit-all-errors %t1.bc 2>&1 | FileCheck %s

// This test tries allocating 8 bytes and it is trying to
// write into addresses 0x80(128) and 0x84(132), but fails on 0x88(136)
// This is an extension of DefineFixedObject.c test

#include <stdio.h>

#define ADDRESS ((int*) 0x0080)

int main() {
  klee_define_fixed_object(ADDRESS, 8);
  int *p = ADDRESS;
  p[1] = 10;
  printf("*p: %d\n", *p);
	printf("p[1]: %d\n", p[1]);
	
	p[2] = 9; // CHECK: memory error

  return 0;
}
