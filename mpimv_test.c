#include "mpiedupack.h"

/* This is a test program which uses mpimv to multiply a
   sparse matrix A and a dense vector u to obtain a dense vector v.
   The sparse matrix and its distribution
   are read from an input file.
   The dense vector v is initialized by the test program.
   The distribution of v is read from an input file.
   The distribution of u is read from another input file.

   The output vector is defined by
       u[i]= (sum: 0<=j<n: a[i][j]*v[j]).
*/

#define DIV 0
#define MOD 1
#define NITERS 1000
#define STRLEN 100

void mpiinput2triple(int p, int s, const char *filename, int *pnA, int *pnz,
                     int **pia, int **pja, double **pa) {

  /* This function reads a sparse matrix in distributed
     Matrix Market format without the banner line
     from the input file and distributes
     matrix triples to the processors.
     The input consists of one line
         m n nz p  (number of rows, columns, nonzeros, processors)
     followed by p+1 lines with the starting numbers
     of the processor parts
         Pstart[0]
         Pstart[1]
         ...
         Pstart[p]
     which means that processor q will get all nonzeros
     numbered Pstart[q]..Pstart[q+1]-1.
     This is followed by nz lines in the format
         i j a     (row index, column index, numerical value).
     The input indices are assumed by Matrix Market to start
     counting at one, but they are converted to start from zero.
     The triples are stored into three arrays ia, ja, a,
     in arbitrary order.

     Input:
     p is the number of processors.
     s is the processor number, 0 <= s < p.

     Output:
     nA is the global matrix size.
     nz is the number of local nonzeros.
     a[k] is the numerical value of the k'th local nonzero,
          0 <= k < nz.
     ia[k] is the global row index of the  k'th local nonzero.
     ja[k] is the global column index.
  */

  int pA, mA, nA, nzA, nz, q, maxnz, k, *Nz, *Pstart, *ia, *ja, *ib, *jb;
  double *a, *b;
  FILE *fp;

  MPI_Status status, status1, status2;

  if (s == 0) {
    fp = fopen(filename, "r");

    /* A is an mA by nA matrix with nzA nonzeros
       distributed over pA processors. */
    fscanf(fp, "%d %d %d %d\n", &mA, &nA, &nzA, &pA);
    if (pA != p)
      MPI_Abort(MPI_COMM_WORLD, -8);
    if (mA != nA)
      MPI_Abort(MPI_COMM_WORLD, -9);

    Nz = vecalloci(p);
    Pstart = vecalloci(p + 1);
    for (q = 0; q <= p; q++)
      fscanf(fp, "%d\n", &Pstart[q]);
    maxnz = 0;
    for (q = 0; q < p; q++) {
      Nz[q] = Pstart[q + 1] - Pstart[q];
      maxnz = MAX(maxnz, Nz[q]);
    }
    vecfreei(Pstart);
  }

  MPI_Bcast(&nA, 1, MPI_INT, 0, MPI_COMM_WORLD);
  MPI_Scatter(Nz, 1, MPI_INT, &nz, 1, MPI_INT, 0, MPI_COMM_WORLD);

  /* Handle the processors one at a time.
     This saves buffer memory, at the expense of extra syncs.
     Buffer memory needed for communication is at most the maximum
     amount of memory a processor needs to store its vector components. */

  a = vecallocd(nz + 1);
  ia = vecalloci(nz + 1);
  ja = vecalloci(nz + 1);

  if (s == 0) {
    /* Allocate temporary memory for input */
    b = vecallocd(maxnz);
    ib = vecalloci(maxnz);
    jb = vecalloci(maxnz);

    /* Read the nonzeros of P(0) from the matrix file and
       store them locally */
    for (k = 0; k < nz; k++) {
      fscanf(fp, "%d %d %lf\n", &ia[k], &ja[k], &a[k]);
      /* Convert indices to range 0..n-1, assuming it was 1..n */
      ia[k]--;
      ja[k]--;
    }
  }

  for (q = 1; q < p; q++) {
    if (s == 0) {
      /* Read the nonzeros of P(q) from the matrix file and
         send them to their destination */
      for (k = 0; k < Nz[q]; k++) {
        fscanf(fp, "%d %d %lf\n", &ib[k], &jb[k], &b[k]);
        ib[k]--;
        jb[k]--;
      }
      MPI_Send(ib, Nz[q], MPI_INT, q, 0, MPI_COMM_WORLD);
      MPI_Send(jb, Nz[q], MPI_INT, q, 1, MPI_COMM_WORLD);
      MPI_Send(b, Nz[q], MPI_DOUBLE, q, 2, MPI_COMM_WORLD);
    } else if (s == q) {
      /* Receive the local nonzeros */
      MPI_Recv(ia, nz, MPI_INT, 0, 0, MPI_COMM_WORLD, &status);
      MPI_Recv(ja, nz, MPI_INT, 0, 1, MPI_COMM_WORLD, &status1);
      MPI_Recv(a, nz, MPI_DOUBLE, 0, 2, MPI_COMM_WORLD, &status2);
    }
  }
  if (s == 0) {
    vecfreei(jb);
    vecfreei(ib);
    vecfreed(b);
    vecfreei(Nz);
  }

  *pnA = nA;
  *pnz = nz;
  *pa = a;
  *pia = ia;
  *pja = ja;
  if (s == 0)
    fclose(fp);

} /* end mpiinput2triple */

int key(int i, int radix, int keytype) {
  /* This function computes the key of an index i
     according to the keytype */

  if (keytype == DIV)
    return i / radix;
  else /* keytype=MOD */
    return i % radix;

} /* end key */

void sort(int n, int nz, int *ia, int *ja, double *a, int radix, int keytype) {
  /* This function sorts the nonzero elements of an n by n sparse
     matrix A stored in triple format in arrays ia, ja, a.
     The sort is by counting.
     If keytype=DIV, the triples are sorted by increasing value of
     ia[k] div radix.
     if keytype=MOD, the triples are sorted by increasing value of
     ia[k] mod radix.
     The sorting is stable: ties are decided so that the original
     precedences are maintained. For a complete sort by increasing
     index ia[k], this function should be called twice:
     first with keytype=MOD, then with keytype=DIV.

     Input:
     n is the global size of the matrix.
     nz is the local number of nonzeros.
     a[k] is the numerical value of the k'th nonzero of the
          sparse matrix A, 0 <= k < nz.
     ia[k] is the global row index of the k'th nonzero.
     ja[k] is the global column index of the k'th nonzero.
     radix >= 1.

     Output: ia, ja, a in sorted order.
  */

  int key(int i, int radix, int keytype);

  int *ia1, *ja1, nbins, *startbin, *lengthbin, r, k, newk;
  double *a1;

  ia1 = vecalloci(nz);
  ja1 = vecalloci(nz);
  a1 = vecallocd(nz);

  /* Allocate bins */
  if (keytype == DIV)
    nbins = (n % radix == 0 ? n / radix : n / radix + 1);
  else if (keytype == MOD)
    nbins = radix;
  startbin = vecalloci(nbins);
  lengthbin = vecalloci(nbins);

  /* Count the elements in each bin */
  for (r = 0; r < nbins; r++)
    lengthbin[r] = 0;
  for (k = 0; k < nz; k++) {
    r = key(ia[k], radix, keytype);
    lengthbin[r]++;
  }

  /* Compute the starting positions */
  startbin[0] = 0;
  for (r = 1; r < nbins; r++)
    startbin[r] = startbin[r - 1] + lengthbin[r - 1];

  /* Enter the elements into the bins in temporary arrays (ia1,ja1,a1) */
  for (k = 0; k < nz; k++) {
    r = key(ia[k], radix, keytype);
    newk = startbin[r];
    ia1[newk] = ia[k];
    ja1[newk] = ja[k];
    a1[newk] = a[k];
    startbin[r]++;
  }

  /* Copy the elements back to the orginal arrays */
  for (k = 0; k < nz; k++) {
    ia[k] = ia1[k];
    ja[k] = ja1[k];
    a[k] = a1[k];
  }

  vecfreei(lengthbin);
  vecfreei(startbin);
  vecfreed(a1);
  vecfreei(ja1);
  vecfreei(ia1);

} /* end sort */

void triple2icrs(int n, int nz, int *ia, int *ja, double *a, int *pnrows,
                 int *pncols, int **prowindex, int **pcolindex) {
  /* This function converts a sparse matrix A given in triple
     format with global indices into a sparse matrix in
     incremental compressed row storage (ICRS) format with
     local indices.

     The conversion needs time and memory O(nz + sqrt(n))
     on each processor, which is O(nz(A)/p + n/p + p).

     Input:
     n is the global size of the matrix.
     nz is the local number of nonzeros.
     a[k] is the numerical value of the k'th nonzero
          of the sparse matrix A, 0 <= k <nz.
     ia[k] is the global row index of the k'th nonzero.
     ja[k] is the global column index of the k'th nonzero.

     Output:
     nrows is the number of local nonempty rows
     ncols is the number of local nonempty columns
     rowindex[i] is the global row index of the i'th
                 local row, 0 <= i < nrows.
     colindex[j] is the global column index of the j'th
                 local column, 0 <= j < ncols.
     a[k] is the numerical value of the k'th local nonzero of the
          sparse matrix A, 0 <= k < nz. The array is sorted by
          row index, ties being decided by column index.
     ia[k] = inc[k] is the increment in the local column index of the
            k'th local nonzero, compared to the column index of the
            (k-1)th nonzero, if this nonzero is in the same row;
            otherwise, ncols is added to the difference.
            By convention, the column index of the -1'th nonzero is 0.
 */

  void sort(int n, int nz, int *ia, int *ja, double *a, int radix, int keytype);

  int radix, i, iglob, iglob_last, j, jglob, jglob_last, k, inck, nrows, ncols,
      *rowindex, *colindex;

  /* radix is the smallest power of two >= sqrt(n)
     The div and mod operations are cheap for powers of two.
     A radix of about sqrt(n) minimizes memory and time. */

  for (radix = 1; radix * radix < n; radix *= 2)
    ;

  /* Sort nonzeros by column index */
  sort(n, nz, ja, ia, a, radix, MOD);
  sort(n, nz, ja, ia, a, radix, DIV);

  /* Count the number of local columns */
  ncols = 0;
  jglob_last = -1;
  for (k = 0; k < nz; k++) {
    jglob = ja[k];
    if (jglob != jglob_last)
      /* new column index */
      ncols++;
    jglob_last = jglob;
  }
  colindex = vecalloci(ncols);

  /* Convert global column indices to local ones.
     Initialize colindex */
  j = 0;
  jglob_last = -1;
  for (k = 0; k < nz; k++) {
    jglob = ja[k];
    if (jglob != jglob_last) {
      colindex[j] = jglob;
      j++;
    }
    ja[k] = j - 1; /* local index of last registered column */
    jglob_last = jglob;
  }

  /* Sort nonzeros by row index using radix-sort */
  sort(n, nz, ia, ja, a, radix, MOD);
  sort(n, nz, ia, ja, a, radix, DIV);

  /* Count the number of local rows */
  nrows = 0;
  iglob_last = -1;
  for (k = 0; k < nz; k++) {
    iglob = ia[k];
    if (iglob != iglob_last)
      /* new row index */
      nrows++;
    iglob_last = iglob;
  }
  rowindex = vecalloci(nrows);

  /* Convert global row indices to local ones.
     Initialize rowindex and inc */
  i = 0;
  iglob_last = -1;
  for (k = 0; k < nz; k++) {
    if (k == 0)
      inck = ja[k];
    else
      inck = ja[k] - ja[k - 1];
    iglob = ia[k];
    if (iglob != iglob_last) {
      rowindex[i] = iglob;
      i++;
      if (k > 0)
        inck += ncols;
    }
    ia[k] = inck; /* ia is used to store inc */
    iglob_last = iglob;
  }
  if (nz == 0)
    ia[nz] = 0;
  else
    ia[nz] = ncols - ja[nz - 1];
  ja[nz] = 0;
  a[nz] = 0.0;

  *pncols = ncols;
  *pnrows = nrows;
  *prowindex = rowindex;
  *pcolindex = colindex;

} /* end triple2icrs */

void mpiinputvec(int p, int s, const char *filename, int *pn, int *pnv,
                 int **pvindex) {

  /* This function reads the distribution of a dense vector v
     from the input file and initializes the corresponding local
     index array.
     The input consists of one line
         n p    (number of components, processors)
     followed by n lines in the format
         i proc (index, processor number),
     where i=1,2,...,n.

     Input:
     p is the number of processors.
     s is the processor number, 0 <= s < p.
     filename is the name of the input file.

     Output:
     n is the global length of the vector.
     nv is the local length.
     vindex[i] is the global index corresponding to
               the local index i, 0 <= i < nv.
  */

  int n, pv, q, b, size, i, j, k, proc, nv, *Tmp, *Tmp2, *Tmp3, *Nsend, *Nrecv,
      *Offset_send, *Offset_recv, *Start, *Nv, *vindex;
  FILE *fp;

  if (s == 0) {
    /* Open the file and read the header */

    fp = fopen(filename, "r");
    fscanf(fp, "%d %d\n", &n, &pv);
    if (pv != p)
      MPI_Abort(MPI_COMM_WORLD, -10);
  }
  MPI_Bcast(&n, 1, MPI_INT, 0, MPI_COMM_WORLD);

  if (s == 0) {
    /* Initialize vector component counter Nv[q] for each P(q) */
    Nv = vecalloci(p);
    for (q = 0; q < p; q++)
      Nv[q] = 0;
  }

  /* Determine block sizes for vector read */
  b = (n % p == 0 ? n / p : n / p + 1); /* batch size */
  size = (b % p == 0 ? b / p : b / p + 1);
  Tmp = vecalloci(3 * p * size);
  Tmp2 = vecalloci(3 * p * size);

  /* Initialize temporary array with dummies */
  for (j = 0; j < 3 * p * size; j++)
    Tmp2[j] = -1;

  /* The owner, global index, and local index
     of each component i is read and stored
     in a distributed temporary array Tmp2. */

  for (q = 0; q < p; q++) {
    /* The components are handled in batches of about n/p
       to save memory. Each batch is read and then immediately
       scattered over the different processors.  */
    if (s == 0) {

      for (j = 0; j < 3 * p * size; j++)
        Tmp[j] = -1;
      j = 0;
      /* Read the vector components from file and store the owner,
         global index, and local index as a triple into Tmp
         on P(0). */
      for (k = q * b; k < (q + 1) * b && k < n; k++) {
        fscanf(fp, "%d %d\n", &i, &proc);
        /* Convert index and processor number to ranges
           0..n-1 and 0..p-1, assuming they were
           1..n and 1..p */
        i--;
        proc--;
        if (i != k)
          MPI_Abort(MPI_COMM_WORLD, -11);
        Tmp[j] = proc;
        j++; /* processor */
        Tmp[j] = i;
        j++; /* global index */
        Tmp[j] = Nv[proc];
        j++; /* local index */
        Nv[proc]++;
      }
    }
    /* Send size triples to each processor. To make the number
       of sends equal, some dummy triples may be sent. */
    MPI_Scatter(Tmp, 3 * size, MPI_INT, &Tmp2[q * 3 * size], 3 * size, MPI_INT,
                0, MPI_COMM_WORLD);
  }

  MPI_Scatter(Nv, 1, MPI_INT, &nv, 1, MPI_INT, 0, MPI_COMM_WORLD);

  /* The global index and local index of each component i are first
     stored as a pair in a distributed temporary array Tmp
     and then sent to its final destination. */

  vindex = vecalloci(nv);
  Nsend = vecalloci(p);
  Nrecv = vecalloci(p);
  Offset_send = vecalloci(p);
  Offset_recv = vecalloci(p);
  Start = vecalloci(p);
  Tmp3 = vecalloci(2 * nv);

  /* Count the number of data to be sent */
  for (q = 0; q < p; q++)
    Nsend[q] = 0;

  for (q = 0; q < p; q++) {
    for (j = q * size; j < (q + 1) * size; j++) {
      proc = Tmp2[3 * j];
      if (proc >= 0)      /* not for dummies */
        Nsend[proc] += 2; /* pairs are sent */
    }
  }

  /* Determine the send offsets */
  Offset_send[0] = 0;
  Start[0] = 0;
  for (q = 1; q < p; q++) {
    Offset_send[q] = Offset_send[q - 1] + Nsend[q - 1];
    Start[q] = Offset_send[q];
  }

  /* Pack the pairs into Tmp, in contiguous blocks,
     one for each destination processor */
  for (q = 0; q < p; q++) {
    for (j = q * size; j < (q + 1) * size; j++) {
      proc = Tmp2[3 * j];
      if (proc >= 0) {
        k = Start[proc];
        Tmp[k] = Tmp2[3 * j + 1];     /* global index */
        Tmp[k + 1] = Tmp2[3 * j + 2]; /* local index */
        Start[proc] += 2;
      }
    }
  }

  /* Derive Nrecv information from Nsend information */
  MPI_Alltoall(Nsend, 1, MPI_INT, Nrecv, 1, MPI_INT, MPI_COMM_WORLD);
  Offset_recv[0] = 0;
  for (q = 1; q < p; q++)
    Offset_recv[q] = Offset_recv[q - 1] + Nrecv[q - 1];

  /* Send pairs of global/local indices */
  MPI_Alltoallv(Tmp, Nsend, Offset_send, MPI_INT, Tmp3, Nrecv, Offset_recv,
                MPI_INT, MPI_COMM_WORLD);

  /* Unpack the global and local indices */
  for (k = 0; k < nv; k++)
    vindex[Tmp3[2 * k + 1]] = Tmp3[2 * k];

  vecfreei(Tmp3);
  vecfreei(Start);
  vecfreei(Offset_recv);
  vecfreei(Offset_send);
  vecfreei(Nrecv);
  vecfreei(Nsend);
  vecfreei(Tmp2);
  vecfreei(Tmp);
  if (s == 0)
    vecfreei(Nv);

  *pn = n;
  *pnv = nv;
  *pvindex = vindex;

} /* end mpiinputvec */

int main(int argc, char **argv) {

  void mpiinput2triple(int p, int s, const char *filename, int *pnA, int *pnz,
                       int **pia, int **pja, double **pa);
  void triple2icrs(int n, int nz, int *ia, int *ja, double *a, int *pnrows,
                   int *pncols, int **prowindex, int **pcolindex);
  void mpiinputvec(int p, int s, const char *filename, int *pn, int *pnv,
                   int **pvindex);
  void mpimv_init(int p, int s, int n, int nrows, int ncols, int nv, int nu,
                  int *rowindex, int *colindex, int *vindex, int *uindex,
                  int *srcprocv, int *srcindv, int *destprocu, int *destindu);
  void mpimv(int p, int s, int n, int nz, int nrows, int ncols, double *a,
             int *inc, int *srcprocv, int *srcindv, int *destprocu,
             int *destindu, int nv, int nu, double *v, double *u);

  int s, p, n, nz, i, iglob, nrows, ncols, nv, nu, iter, *ia, *ja, *rowindex,
      *colindex, *vindex, *uindex, *srcprocv, *srcindv, *destprocu, *destindu;
  double *a, *v, *u, time0, time1, time2;
  char mfilename[STRLEN], vfilename[STRLEN], ufilename[STRLEN];

  MPI_Init(&argc, &argv);

  MPI_Comm_size(MPI_COMM_WORLD, &p); /* p = number of processors */
  MPI_Comm_rank(MPI_COMM_WORLD, &s); /* s = processor number */

  /* Open the matrix file and read the header */
  if (s == 0) {
    printf("Please enter the filename of the matrix distribution\n");
    scanf("%s", mfilename);
  }
  /* Input of sparse matrix */
  mpiinput2triple(p, s, mfilename, &n, &nz, &ia, &ja, &a);

  /* Convert data structure to incremental compressed row storage */
  triple2icrs(n, nz, ia, ja, a, &nrows, &ncols, &rowindex, &colindex);
  vecfreei(ja);

  /* Read vector distributions */
  if (s == 0) {
    printf("Please enter the filename of the v-vector distribution\n");
    scanf("%s", vfilename);
  }
  mpiinputvec(p, s, vfilename, &n, &nv, &vindex);

  if (s == 0) {
    printf("Please enter the filename of the u-vector distribution\n");
    scanf("%s", ufilename);
  }
  mpiinputvec(p, s, ufilename, &n, &nu, &uindex);
  if (s == 0) {
    printf("Sparse matrix-vector multiplication");
    printf(" using %d processors\n", p);
  }

  /* Initialize input vector v */
  v = vecallocd(nv);
  for (i = 0; i < nv; i++) {
    iglob = vindex[i];
    v[i] = iglob + 1;
  }
  u = vecallocd(nu);

  if (s == 0) {
    printf("Initialization for matrix-vector multiplications\n");
    fflush(stdout);
  }
  MPI_Barrier(MPI_COMM_WORLD);
  time0 = MPI_Wtime();

  srcprocv = vecalloci(ncols);
  srcindv = vecalloci(ncols);
  destprocu = vecalloci(nrows);
  destindu = vecalloci(nrows);
  mpimv_init(p, s, n, nrows, ncols, nv, nu, rowindex, colindex, vindex, uindex,
             srcprocv, srcindv, destprocu, destindu);

  if (s == 0) {
    printf("Start of %d matrix-vector multiplications.\n", (int)NITERS);
    fflush(stdout);
  }
  MPI_Barrier(MPI_COMM_WORLD);
  time1 = MPI_Wtime();

  for (iter = 0; iter < NITERS; iter++)
    mpimv(p, s, n, nz, nrows, ncols, a, ia, srcprocv, srcindv, destprocu,
          destindu, nv, nu, v, u);
  MPI_Barrier(MPI_COMM_WORLD);
  time2 = MPI_Wtime();

  if (s == 0) {
    printf("End of matrix-vector multiplications.\n");
    printf("Initialization took only %.6lf seconds.\n", time1 - time0);
    printf("Each matvec took only %.6lf seconds.\n",
           (time2 - time1) / (double)NITERS);
    printf("Total time for %d iterations: %.6lf\n", (int)NITERS,
           (time2 - time1));
    fflush(stdout);
  }

  /* printf("The computed solution is:\n");
  for (i = 0; i < nu; i++) {
    iglob = uindex[i];
    printf("proc=%d i=%d, u=%lf \n", s, iglob, u[i]);
  } */

  vecfreei(destindu);
  vecfreei(destprocu);
  vecfreei(srcindv);
  vecfreei(srcprocv);
  vecfreed(u);
  vecfreed(v);
  vecfreei(uindex);
  vecfreei(vindex);
  vecfreei(rowindex);
  vecfreei(colindex);
  vecfreei(ia);
  vecfreed(a);

  MPI_Finalize();

  exit(0);

} /* end main */
