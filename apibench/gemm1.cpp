#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>

#include <CL/cl.h>


#ifdef MIOPENGEMM_BENCH_ISAAC
#include <clBLAS.h>
#endif

#ifdef MIOPENGEMM_BENCH_CLBLAST
#include <clblast_c.h>
#endif

#include <miopengemm/cpugemm.hpp>
#include <miopengemm/gemm.hpp>
#include <miopengemm/geometries.hpp>
#include <miopengemm/geometry.hpp>
#include <miopengemm/oclutil.hpp>
#include <miopengemm/setabcw.hpp>
#include <miopengemm/stringutilbase.hpp>
#include <miopengemm/timer.hpp>

enum class Impl
{
  MIO = 0,  // MIOpenGEMM
  ISAAC,    // Isaac
  CLB,      // CLBlast
};

class RunStats
{
  public:
  size_t              n_runs;
  double              host_time;
  std::vector<double> event_times;

  RunStats(size_t n_runs_, double host_time_, const std::vector<double>& event_times_)
    : n_runs(n_runs_), host_time(host_time_), event_times(event_times_)
  {
  }
};

template <typename TFloat>
std::map<std::string, RunStats>
runem(std::vector<MIOpenGEMM::Geometry>& geometries,         // GEMM geometries to run
      Impl                               impl,               // GEMM implementer
      bool                               run_accuracy_test,  // confirm correctness
      bool                               run_event_timer)    // get exact times on device
{

  std::map<std::string, RunStats> results;

  namespace Mat     = MIOpenGEMM::Mat;
  namespace Mem     = MIOpenGEMM::Mem;
  namespace oclutil = MIOpenGEMM::oclutil;

  const TFloat alpha = 0.72435898234;
  const TFloat beta  = 0.9241223982;

  auto toff = MIOpenGEMM::get_zero_offsets();

  MIOpenGEMM::owrite::Writer mowri(MIOpenGEMM::Ver::E::SILENT, "");

  MIOpenGEMM::CLHint devhint;

  cl_command_queue_properties cqps = 0;  // CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE;
  if (run_event_timer == true)
  {
    cqps |= CL_QUEUE_PROFILING_ENABLE;
  }

  oclutil::CommandQueueInContext cqic(mowri, cqps, devhint, "(runem)");

  srand(1011);
  MIOpenGEMM::setabcw::CpuMemBundle<TFloat> cmb(geometries, toff);
  std::vector<TFloat>                       c_from_device(cmb.v_mem[Mat::E::C]->size());


  std::cout << "*****************\n";
  switch (impl)
  {
  case Impl::MIO: std::cout << "with MIOpenGEMM\n"; break;
  case Impl::CLB: std::cout << "with CLBlast\n"; break;
  case Impl::ISAAC: std::cout << "with Isaac\n"; break;
  }
  std::cout << "*****************\n";

  for (size_t geomi = 0; geomi < geometries.size(); ++geomi)
  {

    auto& gg = geometries[geomi];

#ifdef MIOPENGEMM_BENCH_CLBLAST
    auto clblast_layout = gg.isColMajor ? CLBlastLayoutColMajor : CLBlastLayoutRowMajor;
    auto clblast_atrans = gg.tX[Mat::E::A] ? CLBlastTransposeYes : CLBlastTransposeNo;
    auto clblast_btrans = gg.tX[Mat::E::B] ? CLBlastTransposeYes : CLBlastTransposeNo;
#endif

#ifdef MIOPENGEMM_BENCH_ISAAC
    auto clblas_layout = gg.isColMajor ? clblasColumnMajor : clblasRowMajor;
    auto clblas_atrans = gg.tX[Mat::E::A] ? clblasTrans : clblasNoTrans;
    auto clblas_btrans = gg.tX[Mat::E::B] ? clblasTrans : clblasNoTrans;
#endif

    std::cout << '(' << geomi << '/' << geometries.size() << ")   " << gg.get_string() << '\n';
    if (MIOpenGEMM::get_floattype_char<TFloat>() != gg.floattype)
    {
      throw MIOpenGEMM::miog_error("incorrect float type in runem.");
    }

    std::vector<double> event_timer_times;
    double              sum_event_times = 0;

    // .............................. set up memories .....................................
    auto memsize = [&gg, &toff](Mat::E emat) {
      return MIOpenGEMM::get_mat_memsize(gg, toff, emat);
    };
    std::array<cl_mem, Mat::E::N> dev_mem;
    cl_mem dev_w = nullptr;
    for (auto x : {Mat::E::A, Mat::E::B, Mat::E::C})
    {
      oclutil::cl_set_buffer_from_command_queue(
        dev_mem[x], cqic.command_queue, CL_MEM_READ_WRITE, memsize(x), NULL, "(runem)", true);

      oclutil::cl_enqueue_write_buffer(cqic.command_queue,
                                       dev_mem[x],
                                       CL_TRUE,
                                       0,
                                       memsize(x),
                                       cmb.r_mem[x],
                                       0,
                                       NULL,
                                       NULL,
                                       "(runem)",
                                       true);
    }
    auto w_mem_size = MIOpenGEMM::get_total_workspace(gg, toff) * gg.derived.float_size_bytes;
    if (w_mem_size > 0)
    {
      oclutil::cl_set_buffer_from_command_queue(
        dev_w, cqic.command_queue, CL_MEM_READ_WRITE, w_mem_size, NULL, " ", true);
    }
    // ......................................................................................

    // number of runs before starting timer
    size_t n_warmup = 1;
    // number of runs with timer (based on DeepBench timing method).
    size_t n_to_time =
      std::min<size_t>(1000, std::max<size_t>(std::ceil(1e11 / (2 * gg.m * gg.k * gg.n)), 2));
    size_t n_total = n_warmup + n_to_time;

    MIOpenGEMM::Timer timer;
    
    int               miog_gemm_ID = -1;
    
    for (size_t run_i = 0; run_i < n_total; ++run_i)
    {
      
      if (run_i == n_warmup)
      {
        timer.start();
      }

      cl_event  base_gemmevent;
      auto      use_cl_event  = (run_i < n_warmup || run_event_timer || run_i == n_total - 1);
      cl_event* ptr_gemmevent = (use_cl_event) ? &base_gemmevent : nullptr;

      // MIOpenGEMM GEMM
      if (impl == Impl::MIO)
      {

        MIOpenGEMM::GemmStatus mi_status = MIOpenGEMM::xgemm<TFloat>(gg.isColMajor,
                                                                     gg.tX[Mat::E::A],
                                                                     gg.tX[Mat::E::B],
                                                                     gg.m,
                                                                     gg.n,
                                                                     gg.k,
                                                                     alpha,
                                                                     dev_mem[Mat::E::A],
                                                                     toff.offsets[Mem::E::A],
                                                                     gg.ldX[Mat::E::A],
                                                                     dev_mem[Mat::E::B],
                                                                     toff.offsets[Mem::E::B],
                                                                     gg.ldX[Mat::E::B],
                                                                     beta,
                                                                     dev_mem[Mat::E::C],
                                                                     toff.offsets[Mem::E::C],
                                                                     gg.ldX[Mat::E::C],
                                                                     dev_w,
                                                                     toff.offsets[Mem::E::W],
                                                                     gg.wSpaceSize,
                                                                     &cqic.command_queue,
                                                                     0,
                                                                     nullptr,
                                                                     ptr_gemmevent,
                                                                     miog_gemm_ID);

        miog_gemm_ID = mi_status.ID;
        (void)mi_status;
      }

      
#ifdef MIOPENGEMM_BENCH_CLBLAST      
      else if (impl == Impl::CLB)
      {
        CLBlastStatusCode status = CLBlastSgemm(clblast_layout,
                                                clblast_atrans,
                                                clblast_btrans,
                                                gg.m,
                                                gg.n,
                                                gg.k,
                                                alpha,
                                                dev_mem[Mat::E::A],
                                                toff.offsets[Mem::E::A],
                                                gg.ldX[Mat::E::A],
                                                dev_mem[Mat::E::B],
                                                toff.offsets[Mem::E::B],
                                                gg.ldX[Mat::E::B],
                                                beta,
                                                dev_mem[Mat::E::C],
                                                toff.offsets[Mem::E::C],
                                                gg.ldX[Mat::E::C],
                                                &cqic.command_queue,
                                                ptr_gemmevent);
        (void)status;
      }
#endif


#ifdef MIOPENGEMM_BENCH_ISAAC
      else if (impl == Impl::ISAAC)
      {

        clblasStatus isaac_status = clblasSgemm(clblas_layout,
                                                clblas_atrans,
                                                clblas_btrans,
                                                gg.m,
                                                gg.n,
                                                gg.k,
                                                alpha,
                                                dev_mem[Mat::E::A],
                                                toff.offsets[Mem::E::A],
                                                gg.ldX[Mat::E::A],
                                                dev_mem[Mat::E::B],
                                                toff.offsets[Mem::E::B],
                                                gg.ldX[Mat::E::B],
                                                beta,
                                                dev_mem[Mat::E::C],
                                                toff.offsets[Mem::E::C],
                                                gg.ldX[Mat::E::C],
                                                1,
                                                &cqic.command_queue,
                                                0,
                                                nullptr,
                                                ptr_gemmevent);

        (void)isaac_status;
      }
#endif

      else
      {
        std::stringstream ss;
        ss << "unknown Impl " << static_cast<int>(impl) << '\n';
        throw MIOpenGEMM::miog_error(ss.str());
      }

      if (use_cl_event)
      {
        clWaitForEvents(1, ptr_gemmevent);
      }

      // perform accuracy test if first run and accuracy test requested
      if (run_i == 0 && run_accuracy_test)
      {
        cl_event readevent;
        oclutil::cl_enqueue_read_buffer(cqic.command_queue,
                                        dev_mem[Mat::E::C],
                                        CL_TRUE,
                                        0,
                                        memsize(Mat::E::C),
                                        c_from_device.data(),
                                        0,
                                        NULL,
                                        &readevent,
                                        "(runem)",
                                        true);

        // openblas (assuming OPENBLAS ON when MIOpenGEMM was built)
        std::vector<TFloat> c_cpu(*cmb.v_mem[Mat::E::C]);
        MIOpenGEMM::cpugemm::gemm<TFloat>(
          gg, toff, cmb.r_mem[Mat::E::A], cmb.r_mem[Mat::E::B], c_cpu.data(), alpha, beta, mowri);

        oclutil::cl_wait_for_events(1, &readevent, "(runem)", true);

        TFloat sum_gpu = std::accumulate(c_from_device.begin(), c_from_device.end(), TFloat(0));
        TFloat sum_cpu = std::accumulate(c_cpu.begin(), c_cpu.end(), TFloat(0));
        std::cout << "sum C [gpu]: " << sum_gpu << ",  sum C [cpu]: " << sum_cpu << std::endl;
      }

      if (run_event_timer && run_i >= n_warmup)
      {

        size_t t_start;
        oclutil::cl_set_event_profiling_info(*ptr_gemmevent,
                                             CL_PROFILING_COMMAND_START,
                                             sizeof(size_t),
                                             &t_start,
                                             nullptr,
                                             "(runem)",
                                             true);

        size_t t_end;
        oclutil::cl_set_event_profiling_info(*ptr_gemmevent,
                                             CL_PROFILING_COMMAND_END,
                                             sizeof(size_t),
                                             &t_end,
                                             nullptr,
                                             "(runem)",
                                             true);

        event_timer_times.push_back(1e-6 * (t_end - t_start));
        sum_event_times += event_timer_times.back();

        if (run_i == n_warmup)
        {
          std::cout << "cl event \"gflops\" : [";
        }

        std::cout << ' ' << gg.get_gflops(1e-3 * event_timer_times.back()) << ' ' << std::flush;

        if (run_i == n_total - 1)
        {
          std::cout << ']';
          std::cout << "\nmean event time : " << sum_event_times / n_to_time << std::endl;
        }
      }

      if (use_cl_event)
      {
        oclutil::cl_release_event(*ptr_gemmevent, "from VeriGEMM", true);
      }
    }
    
    // overall timer
    auto   t_total   = 1000 * timer.get_elapsed();
    double mean_time = t_total / n_to_time;
    double gflops    = gg.get_gflops(1e-3 * mean_time);
    std::cout << "total host time : " << t_total << " [ms] total runs : " << n_to_time << std::endl;
    std::cout << "mean  host time : " << mean_time << "   mean gflops : " << gflops << std::endl;

    if (run_event_timer)
    {
      std::cout << "mean  time diff (event - host) " << mean_time - sum_event_times / n_to_time
                << " [ms] " << std::endl;
    }
    results.insert(
      std::make_pair(gg.get_string(), RunStats(n_to_time, t_total / 1000., event_timer_times)));

    std::cout << '\n';
    // Clean-up
    for (auto x : {Mat::E::A, Mat::E::B, Mat::E::C})
    {
      clReleaseMemObject(dev_mem[x]);
    }
    clReleaseMemObject(dev_w);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
  return results;
}

void neat_print(const std::map<std::string, RunStats>& results)
{
  std::cout << "\n";
  for (auto& x : results)
  {
    MIOpenGEMM::Geometry gg(std::get<0>(x));
    RunStats             runstats(std::get<1>(x));
    auto                 time = runstats.host_time / runstats.n_runs;
    std::cout << gg.get_tabbed_string()
              << "  time[ms]:" << MIOpenGEMM::stringutil::get_char_padded(time * 1000, 10)
              << "  gflops:" << gg.get_gflops(time) << std::endl;
  }
  
  std::cout << "\n\n";
}

int main()
{

  bool run_accuracy_test = false;
  bool run_event_timer   = false;

  // auto gg = MIOpenGEMM::get_tight_geometry<float>(true, false, false, false, 5100, 5100, 5100,
  // 0);

  std::vector<MIOpenGEMM::Geometry> deepbench_geometries = MIOpenGEMM::get_deepbench(0);  //{gg};
  // Tracking issues observed with DeepBench problems.
  // Issues observed in Isaac   :  https://github.com/ptillet/isaac/issues/26
  // Issues observed in CLBlast :  https://github.com/CNugteren/CLBlast/issues/185
  std::array<std::vector<MIOpenGEMM::Geometry>, 10> problems;
  problems[static_cast<int>(Impl::ISAAC)] = {
    // DeepBench memory issues (on ROCm 1.6)
    {"tC0_tA0_tB0_colMaj1_m35_n8457_k4096_lda35_ldb4096_ldc35_ws0_f32"},
    {"tC0_tA0_tB0_colMaj1_m35_n8457_k2048_lda35_ldb2048_ldc35_ws0_f32"},
    {"tC0_tA0_tB1_colMaj1_m2048_n7133_k2048_lda2048_ldb7133_ldc2048_ws0_f32"},
    {"tC0_tA0_tB1_colMaj1_m3072_n7435_k1024_lda3072_ldb7435_ldc3072_ws0_f32"},
    {"tC0_tA0_tB1_colMaj1_m4096_n7133_k4096_lda4096_ldb7133_ldc4096_ws0_f32"}};
  
  problems[static_cast<int>(Impl::CLB)] = {
    // Either 
    // (1) Excessive memory on (ROCm 1.6) or 
    // (2) Memory out of bounds. 
    {"tC0_tA1_tB0_colMaj1_m512_n8_k500000_lda500000_ldb500000_ldc512_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m1024_n8_k500000_lda500000_ldb500000_ldc1024_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m512_n16_k500000_lda500000_ldb500000_ldc512_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m1024_n16_k500000_lda500000_ldb500000_ldc1024_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m4096_n16_k4096_lda4096_ldb4096_ldc4096_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m2048_n32_k2048_lda2048_ldb2048_ldc2048_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m6144_n16_k2048_lda2048_ldb2048_ldc6144_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m8448_n16_k2816_lda2816_ldb2816_ldc8448_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m7680_n16_k2560_lda2560_ldb2560_ldc7680_ws0_f32"},
    {"tC0_tA0_tB0_colMaj1_m35_n8457_k2048_lda35_ldb2048_ldc35_ws0_f32"},
    {"tC0_tA1_tB0_colMaj1_m35_n8457_k2048_lda2048_ldb2048_ldc35_ws0_f32"}};

  problems[static_cast<int>(Impl::MIO)] = {
    
  };

  // TODO : make runtime option as well 
  for (auto impl : {
#ifdef MIOPENGEMM_BENCH_CLBLAST    
    Impl::CLB,
#endif
#ifdef MIOPENGEMM_BENCH_ISAAC
    Impl::ISAAC,
#endif
    Impl::MIO
    })
  {
    
    // The geometries to run.
    std::vector<MIOpenGEMM::Geometry> geometries;

    bool run_deepbench_geometries = false;
    if (run_deepbench_geometries){
      for (unsigned i = 0; i < deepbench_geometries.size(); ++i)
      {
        auto gg       = deepbench_geometries[i];
        auto impl_int = static_cast<int>(impl);
        if (std::find(problems[impl_int].begin(), problems[impl_int].end(), gg) ==
            problems[impl_int].end())
        {
          geometries.push_back(gg);
        }
      }
    }
    
    // custom geometries. 
    else{
      geometries = {{"tC0_tA1_tB0_colMaj1_m433_n117_k4000_lda5024_ldb5024_ldc5025_ws0_f32"},
                    {"tC0_tA0_tB1_colMaj1_m4000_n1000_k65_lda5024_ldb5024_ldc5025_ws0_f32"}};
      //geometries = {MIOpenGEMM::get_squareNN_geometry<float>(10)};
    }

    auto stats = runem<float>(geometries, impl, run_accuracy_test, run_event_timer);
    neat_print(stats);
  }
  return 0;
}