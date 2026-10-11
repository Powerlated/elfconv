/* Standalone runner for vendored glibc test cases.  The normal glibc driver
   provides process supervision and command-line options, which run_i386.py
   does not use.  Keep test entry points and preparation/cleanup semantics.  */
#include <support/check.h>
#include <support/test-driver.h>

const char *test_dir = ".";
unsigned int test_verbose;

int
main (int argc, char **argv)
{
#ifdef PREPARE
  PREPARE (argc, argv);
#endif

  int result;
#ifdef TEST_FUNCTION_ARGV
  result = TEST_FUNCTION_ARGV (argc, argv);
#elif defined (TEST_FUNCTION)
  result = TEST_FUNCTION;
#else
  result = do_test ();
#endif

#ifdef CLEANUP_HANDLER
  CLEANUP_HANDLER ();
#endif

  return support_report_failure != NULL ? support_report_failure (result) : result;
}
