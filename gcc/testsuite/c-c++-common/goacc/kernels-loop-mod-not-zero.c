/* { dg-additional-options "-O2" } */
/* { dg-additional-options "-ftree-parallelize-loops=32" } */
/* { dg-additional-options "-fdump-tree-parloops1-all" } */
/* { dg-additional-options "-fdump-tree-optimized" } */

#include <stdlib.h>

#define N ((1024 * 512) + 1)
#define COUNTERTYPE unsigned int

int
main (void)
{
  unsigned int *__restrict a;
  unsigned int *__restrict b;
  unsigned int *__restrict c;

  a = (unsigned int *__restrict)malloc (N * sizeof (unsigned int));
  b = (unsigned int *__restrict)malloc (N * sizeof (unsigned int));
  c = (unsigned int *__restrict)malloc (N * sizeof (unsigned int));

  for (COUNTERTYPE i = 0; i < N; i++)
    a[i] = i * 2;

  for (COUNTERTYPE i = 0; i < N; i++)
    b[i] = i * 4;

#pragma acc kernels copyin (a[0:N], b[0:N]) copyout (c[0:N])
  {
    for (COUNTERTYPE ii = 0; ii < N; ii++)
      c[ii] = a[ii] + b[ii];
  }

  for (COUNTERTYPE i = 0; i < N; i++)
    if (c[i] != a[i] + b[i])
      abort ();

  free (a);
  free (b);
  free (c);

  return 0;
}

/* Check that only one loop is analyzed, and that it can be parallelized.  */
/* { dg-final { scan-tree-dump-times "SUCCESS: may be parallelized" 1 "parloops1" } } */
/* { dg-final { scan-tree-dump-not "FAILED:" "parloops1" } } */

/* Check that the loop has been split off into a function.  */
/* { dg-final { scan-tree-dump-times "(?n);; Function .*main._omp_fn.0" 1 "optimized" } } */

/* { dg-final { scan-tree-dump-times "(?n)oacc function \\(32," 1 "parloops1" } } */
