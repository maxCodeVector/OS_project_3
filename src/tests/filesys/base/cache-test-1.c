#include <random.h>
#include <stdlib.h>
#include <stdio.h>
#include <syscall.h>
#include "tests/lib.h"
#include "tests/filesys/base/syn-write.h"

char buf[BUF_SIZE];

int
test_main (int argc, char *argv[])
{
  int tmp = 0;
  CHECK (mkdir ("a"), "mkdir \"a\"");
  CHECK (create ("a/b", 512), "create \"a/b\"");
  tmp = dirty_number();
  CHECK (tmp <= 64, "cache number <= 64");
}
