// Smoke-test file: deliberate clang-format violations.
// This file is temporary and will be reverted after CI confirms
// the workspace-file pipeline detects formatting issues correctly.
#include <stdio.h>

// Wrong indentation (2-space instead of 4-space per .clang-format)
void smoke_test_fmt() {
  int x = 1;
  int y = 2;
  if(x == y){
    printf("equal\n");
  }
}
