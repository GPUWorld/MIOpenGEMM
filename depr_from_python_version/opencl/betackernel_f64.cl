/* This kernel is NOT generated by a Python script! 
 * It is used to perform the beta*C step in GEMM, 
 * where recall GEMM has C <- alpha*A*B + beta*C
 * It is not simplt an axpy, as when ldc is not minimal, 
 * C is not a cotiguous chunk of memory.  */

/* The number of values from C which each a non-edge work-item will scale be beta */
#define WORK_PER_THREAD 4

/* TODO : does nvidia support this? Will Navi support this? */
#define N_WORK_ITEMS_PER_GROUP 64

/* TODO : figure out where to set TFLOAT (as per _32 kernel) */
#define TFLOAT double

__attribute__((reqd_work_group_size(N_WORK_ITEMS_PER_GROUP,1,1)))
__kernel void heeltemal(const unsigned dim_coal, const unsigned dim_uncoal, const unsigned ldc, const unsigned c_offset, __global TFLOAT * c, TFLOAT beta){
/* n_work_groups : number of work groups (determined by host from dimensions of the problem)
 * dim_coal : less than or equal to ldc, this is size in the contiguous direction (m for c matrix if col contiguous and not transposed) 
 * dim_uncol : the other dimension of the matrix */


  c += c_offset;
 
  unsigned group_id = get_group_id(0);
  unsigned local_id = get_local_id(0);
  unsigned global_id = group_id*N_WORK_ITEMS_PER_GROUP + local_id; 
  
  unsigned n_full_work_items_per_line = dim_coal / WORK_PER_THREAD;
  unsigned n_work_items_per_line = n_full_work_items_per_line + (dim_coal % WORK_PER_THREAD != 0);
  
  unsigned n_full_work_items = n_full_work_items_per_line*dim_uncoal;
  unsigned n_work_items = n_work_items_per_line*dim_uncoal;
  
  unsigned start_uncoal = 0;
  unsigned start_coal = 0;

  bool is_in_full_zone = (global_id < n_full_work_items);
  if (is_in_full_zone){   
    start_uncoal = global_id / n_full_work_items_per_line;
    start_coal = WORK_PER_THREAD * (global_id % n_full_work_items_per_line);
  }
  
  else if (global_id < n_work_items){
    start_uncoal = (global_id - n_full_work_items)% dim_uncoal;
    start_coal = WORK_PER_THREAD*n_full_work_items_per_line;
  }

  c += start_uncoal * ldc;
  c += start_coal;

  if (is_in_full_zone){
    #pragma unroll WORK_PER_THREAD
    for (unsigned i = 0; i < WORK_PER_THREAD; ++i){
      c[i] *= beta;
    }
  }
  
  else if (global_id < n_work_items){
    for (unsigned i = 0; i < (dim_coal % WORK_PER_THREAD); ++i){
      c[i] *= beta;
    }
  }
}
