#ifndef GUARD_MIOPENGEMM_JINX_HPP
#define GUARD_MIOPENGEMM_JINX_HPP

/*******************************************************************************
 * Copyright (C) 2017 Advanced Micro Devices, Inc. All rights reserved.
 *******************************************************************************/
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>
#include <miopengemm/architests.hpp>
#include <miopengemm/bundle.hpp>
#include <miopengemm/derivedparams.hpp>
#include <miopengemm/error.hpp>
#include <miopengemm/findparams.hpp>
#include <miopengemm/graph.hpp>
#include <miopengemm/kernel.hpp>
#include <miopengemm/kernelcache.hpp>
#include <miopengemm/kernelstring.hpp>
#include <miopengemm/oclutil.hpp>
#include <miopengemm/outputwriter.hpp>
#include <miopengemm/solution.hpp>
#include <miopengemm/stringutilbase.hpp>

namespace MIOpenGEMM
{

class MFType
{
  private:
  double v_d;
  float  v_f;

  public:
  MFType(double v);
  void* operator[](char floattype) const;
};

static const MFType m_alpha(default_alpha);
static const MFType m_beta(default_beta);

class GpuMms
{
  private:
  std::array<cl_mem, Mem::E::N> cl_mems;
  oclutil::SafeClMem c_copy{"to be used in the case that c_is_const"};

  public:
  GpuMms(cl_mem           a_gpu_,
         cl_mem           b_gpu_,
         cl_mem           c_gpu_,
         bool             c_is_const,
         cl_mem           workspace_gpu_,
         size_t           c_nbytes,
         cl_command_queue cq);
  cl_mem& operator[](Mem::E x);
};

class Jinx
{

  public:
  Jinx(cl_command_queue command_queue_,
       const Geometry   gg_,
       const Offsets    toff_,
       cl_mem           a_gpu_,
       cl_mem           b_gpu_,
       cl_mem           c_gpu_,
       bool             c_is_const,
       cl_mem           workspace_gpu_,
       owrite::Writer&  mowri_);

  //  Jinx(owrite::Writer& mowri_): gg(get_null_geometry()), toff(get_zero_offsets()), mowri(mowri_)
  //  {}

  void benchgemm(const HyPas& hp, size_t max_n_runs, double max_time);
  Solution find(const Constraints& constraint, const FindParams& find_params);

  ///////////////////////////////////////////////////////////////////////////////////////////

  private:
  cl_command_queue       command_queue;
  std::string            outputfilename;
  const Geometry         gg;
  const Offsets          toff;
  GpuMms                 gpum;
  const oclutil::DevInfo devinfo;
  owrite::Writer&        mowri;
  // special purpose output writer
  // vector of times over a set of runs on core loop
  std::vector<float> v_t_total;
  float              median_time;
  float              median_gflops;
  // (for single_descent_find) while generating, compiling and
  // benchmarking kernels, we will keep track of the
  // fastest found thus far
  std::vector<Solution>                                       best_solns_path;
  std::vector<Kernel>                                         tk_kernels;
  std::vector<Kernel*>                                        tk_kernels_active;
  std::vector<std::vector<size_t>>                            v_wait_indices;
  bool                                                        bundle_verbose{false};
  float                                                       total_elapsed_seconds{0};
  size_t                                                      total_elapsed_descents{0};
  size_t                                                      total_kernels_tested{0};
  std::chrono::time_point<std::chrono::high_resolution_clock> find_start;
  std::string                                                 old_comment_string;
  std::string                                                 new_comment_string;

  // TODO : floats to doubles
  float get_gflops(float timems);

  void set_medians();
  void address_check_valid();
  void address_check_valid_and_reliable();
  void run_checks();
  void set_kern_args(const KernelType& type);
  void update_run_times(cl_int status);
  void reset_v_times();
  void mowri_tracker_print();
  void deriveability_test(const HyPas& hp, const std::string& hash);

  bool refresh_needed(BasicKernelType::E type, const HyPas& new_hp, const DerivedParams& new_dp);

  oclutil::Result refresh_kernel(const KernelString& ks, const HyPas& hp, const DerivedParams& dp);
  oclutil::Result setup_tinykernels(const HyPas& hp, const kerngen::Bundle& bundle);
  oclutil::Result core_gemm_loop(size_t max_n_runs, double max_time, bool print_asap);

  std::string get_run_times_heading();
  std::string get_run_time_string(cl_int status);

  // HyPas get_hyper_param_start();
  void     update_total_elapsed_seconds();
  Solution single_descent_find(float allotted_time, const Constraints&, const FindParams&);
};
}

#endif
