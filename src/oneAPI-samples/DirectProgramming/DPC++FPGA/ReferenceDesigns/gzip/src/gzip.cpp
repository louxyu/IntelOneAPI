/**
 * 高宽带版本
 * 高带宽变体最大限度地提高了系统的吞吐量，而不考虑延时。
 * 它将输入/输出SYCL缓冲区转移到与FPGA相连的DDR上。然后，内核对这些缓冲区进行操作
 */
#include <CL/sycl.hpp>
#include <sycl/ext/intel/fpga_extensions.hpp>
#include <chrono>
#include <fstream>
#include <string>

#include "CompareGzip.hpp"
#include "WriteGzip.hpp"
#include "crc32.hpp"
#include "gzipkernel.hpp"
#include "kernels.hpp"

// dpc_common.hpp can be found in the dev-utilities include folder.
// e.g., $ONEAPI_ROOT/dev-utilities//include/dpc_common.hpp
#include "dpc_common.hpp"

using namespace sycl;

// The minimum file size of a file to be compressed.
// Any filesize less than this results in an error.
constexpr int minimum_filesize = kVec + 1;//要压缩文件的最小大小

bool help = false;

int CompressFile(queue &q, std::string &input_file, std::vector<std::string> outfilenames,
                 int iterations, bool report);

void Help(void) {
  // Command line arguments.
  // gzip [options] filetozip [options]
  // -h,--help                    : help

  // future options?
  // -p,performance : output perf metrics
  // -m,maxmapping=#  : maximum mapping size

  std::cout << "gzip filename [options]\n";
  std::cout << "  -h,--help                                : this help text\n";
  std::cout
      << "  -o=<filename>,--output-file=<filename>   : specify output file\n";
}

bool FindGetArg(std::string &arg, const char *str, int defaultval, int *val) {
  std::size_t found = arg.find(str, 0, strlen(str));
  if (found != std::string::npos) {
    int value = atoi(&arg.c_str()[strlen(str)]);
    *val = value;
    return true;
  }
  return false;
}

constexpr int kMaxStringLen = 40;

bool FindGetArgString(std::string &arg, const char *str, char *str_value, size_t maxchars) {
  std::size_t found = arg.find(str, 0, strlen(str));
  if (found != std::string::npos) {
    const char *sptr = &arg.c_str()[strlen(str)];
    for (int i = 0; i < maxchars - 1; i++) {
      char ch = sptr[i];
      switch (ch) {
        case ' ':
        case '\t':
        case '\0':
          str_value[i] = 0;
          return true;
          break;
        default:
          str_value[i] = ch;
          break;
      }
    }
    return true;
  }
  return false;
}

size_t SyclGetExecTimeNs(event e) {
  size_t start_time =
      e.get_profiling_info<info::event_profiling::command_start>();
  size_t end_time =
      e.get_profiling_info<info::event_profiling::command_end>();
  return (end_time - start_time);
}

int main(int argc, char *argv[]) {
  std::string infilename = "";

  std::vector<std::string> outfilenames (kNumEngines);

  char str_buffer[kMaxStringLen] = {0};

  // Check the number of arguments specified
  if (argc != 3) {
    std::cerr << "Incorrect number of arguments. Correct usage: " << argv[0]
              << " <input-file> -o=<output-file>\n";
    return 1;
  }

  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      std::string sarg(argv[i]);
      if (std::string(argv[i]) == "-h") {
        help = true;
      }
      if (std::string(argv[i]) == "--help") {
        help = true;
      }

      FindGetArgString(sarg, "-o=", str_buffer, kMaxStringLen);
      FindGetArgString(sarg, "--output-file=", str_buffer, kMaxStringLen);
    } else {
      infilename = std::string(argv[i]);
    }
  }

  if (help) {
    Help();
    return 1;
  }

  try {
#ifdef FPGA_EMULATOR
    ext::intel::fpga_emulator_selector device_selector;
#else
    ext::intel::fpga_selector device_selector;
#endif
    auto prop_list = property_list{property::queue::enable_profiling()};
    queue q(device_selector, dpc_common::exception_handler, prop_list);

    std::cout << "Running on device:  " << q.get_device().get_info<info::device::name>().c_str() << "\n";

    if (infilename == "") {
      std::cout << "Must specify a filename to compress\n\n";
      Help();
      return 1;
    }
    // next, check valid and acceptable parameter ranges.
    // if output filename not set, use the default
    // name, else use the name specified by the user
    outfilenames[0] = std::string(infilename) + ".gz";
    if (strlen(str_buffer)) {
      outfilenames[0] = std::string(str_buffer);
    }
    for (size_t i=1; i< kNumEngines; i++) {
      // Filenames will be of the form outfilename, outfilename2, outfilename3 etc.
      outfilenames[i] = outfilenames[0] + std::to_string(i+1);
    }

    std::cout << "Launching High-Bandwidth DMA GZIP application with " << kNumEngines
              << " engines\n";

#ifdef FPGA_EMULATOR
    CompressFile(q, infilename, outfilenames, 1, true);
#else
    // warmup run - use this run to warmup accelerator. There are some steps in
    // the runtime that are only executed on the first kernel invocation but not
    // on subsequent invocations. So execute all that stuff here before we
    // measure performance (in the next call to CompressFile().
    CompressFile(q, infilename, outfilenames, 1, false);
    // profile performance
    CompressFile(q, infilename, outfilenames, 200, true);
#endif
  } catch (sycl::exception const &e) {
    // Catches exceptions in the host code
    std::cerr << "Caught a SYCL host exception:\n" << e.what() << "\n";

    // Most likely the runtime couldn't find FPGA hardware!
    if (e.code().value() == CL_DEVICE_NOT_FOUND) {
      std::cerr << "If you are targeting an FPGA, please ensure that your "
                   "system has a correctly configured FPGA board.\n";
      std::cerr << "Run sys_check in the oneAPI root directory to verify.\n";
      std::cerr << "If you are targeting the FPGA emulator, compile with "
                   "-DFPGA_EMULATOR.\n";
    }
    std::terminate();
  }
  return 0;
}

/**
 * 内核信息
 */
struct KernelInfo {
  buffer<struct GzipOutInfo, 1> *gzip_out_buf;//输出缓存数据
  buffer<unsigned, 1> *current_crc;//crc校验数据
  buffer<char, 1> *pobuf;//
  buffer<char, 1> *pibuf;//
  char *pobuf_decompress;//

  uint32_t buffer_crc[kMinBufferSize];//
  uint32_t refcrc;//

  const char *pref_buffer;//
  char *poutput_buffer;//
  size_t file_size;//
  struct GzipOutInfo out_info[kMinBufferSize];//
  int iteration;//
  bool last_block;//
};

// returns 0 on success, otherwise a non-zero failure code.
/**
 *
 * @param q  queue
 * @param input_file 需要压缩的文件
 * @param outfilenames 压缩文件存放路径
 * @param iterations 迭代次数
 * @param report 是否生成压缩文件及报告 true:输出;false:不输出
 * @return
 */
int CompressFile(queue &q, std::string &input_file, std::vector<std::string> outfilenames,int iterations, bool report) {
  size_t isz;//需要压缩文件的大小
  char *pinbuf;//文件缓存 设备可访问主机分配时 可以快速访问DMA

  // Read the input file
  //获取FPGA设备信息
  std::string device_string =q.get_device().get_info<info::device::name>().c_str();

  // If
  // the device is S10, we pre-pin some buffers to
  // improve DMA performance, which is needed to
  // achieve peak kernel throughput. Pre-pinning is
  // only supported on the PAC-S10-USM BSP. It's not
  // needed on PAC-A10 to achieve peak performance.
  //判断是否是S10设备
  bool isS10 =  (device_string.find("s10") != std::string::npos);
  //判断设备是否可以访问主机分配
  bool prepin = q.get_device().get_info<info::device::usm_host_allocations>();

  //是S10设备 但不能访问主机分配时 发出警告
  if (isS10 && !prepin) {
      //警告:此平台不支持主机分配，这意味着不支持预固定。DMA传输可能比预期的要慢，这可能会降低应用程序吞吐量
    std::cout << "Warning: Host allocations are not supported on this platform, which means that pre-pinning is not supported. DMA transfers may be slower than expected which may reduce application throughput.\n\n";
  }


  /*const std::string secret {"Ifmmp-xpsme\")boe!tpnf!beejujpobm!ufyu!mfgu!up!fyqfsjfodf!cz!svoojoh!ju1*"};
   const auto sz=secret.size();
   char*result=sycl::malloc_shared<char>(sz,q);

   std::memcpy(result,secret.data(),sz);

   q.parallel_for(sz,[=](auto&i){
       result[i]-=1;
   }).wait();
   std::cout<< result << "\n";*/





    // padding for the input and output buffers to deal with granularity of
  // kernel reads and writes

  //缓冲区大小256
  constexpr size_t kInOutPadding = 16 * kVec;//缓冲区 值=256
  
  std::ifstream file(input_file,std::ios::in | std::ios::binary | std::ios::ate);
  if (file.is_open()) {
      //获取流指针
    isz = file.tellg();//
    if (prepin) {
      //Pre-pin the buffer, for faster DMA
      //缓存可以快速访问DMA
      pinbuf = (char *)malloc_host(isz + kInOutPadding, q.get_context());
    } else {
        // throughput, using malloc_host().
      pinbuf = new char[isz + kInOutPadding];
    }
    file.seekg(0, std::ios::beg);
    file.read(pinbuf, isz);
    file.close();
  } else {
    std::cout << "Error: cannot read specified input file\n";
    return 1;
  }

  if (isz < minimum_filesize) {
    std::cout << "Minimum filesize for compression is " << minimum_filesize
              << "\n";
    return 1;
  }

  int buffers_count = iterations;//缓存数量 iterations 1/200

  // Create an array of kernel info structures and create buffers for kernel
  // input/output. The buffers are re-used between iterations, but enough 
  // disjoint buffers are created to support double-buffering.
  //初始化内核信息
  struct KernelInfo *kinfo[kNumEngines];//内核信息数组
  for (size_t eng = 0; eng < kNumEngines; eng++) {
    kinfo[eng] =(struct KernelInfo *)malloc(sizeof(struct KernelInfo) * buffers_count);
    if (kinfo[eng] == NULL) {
      std::cout << "Cannot allocate kernel info buffer.\n";
      return 1;
    }
    for (int i = 0; i < buffers_count; i++) {
      kinfo[eng][i].file_size = isz;
      // Allocating slightly larger buffers (+ 16 * kVec) to account for
      // granularity of kernel writes
      int outputSize =((isz + kInOutPadding) < kMinBufferSize) ? kMinBufferSize: (isz + kInOutPadding);
      const size_t input_alloc_size = isz + kInOutPadding;

      // Pre-pin buffer using malloc_host() to improve DMA bandwidth.
      if (i >= 3) {
        kinfo[eng][i].poutput_buffer = kinfo[eng][i - 3].poutput_buffer;
      } else {
        if (prepin) {
          kinfo[eng][i].poutput_buffer =(char *)malloc_host(outputSize, q.get_context());
        } else {
          kinfo[eng][i].poutput_buffer = (char *)malloc(outputSize);
        }
        if (kinfo[eng][i].poutput_buffer == NULL) {
          std::cout << "Cannot allocate output buffer.\n";
          free(kinfo[eng]);
          return 1;
        }
        // zero pages to fully allocate them
        //并行初始化内存
        memset(kinfo[eng][i].poutput_buffer, 0, outputSize);
      }

      kinfo[eng][i].last_block = true;
      kinfo[eng][i].iteration = i;
      kinfo[eng][i].pref_buffer = pinbuf;

      kinfo[eng][i].gzip_out_buf = i >= 3 ? kinfo[eng][i - 3].gzip_out_buf: new buffer<struct GzipOutInfo, 1>(kMinBufferSize);
      kinfo[eng][i].current_crc = i >= 3? kinfo[eng][i - 3].current_crc: new buffer<unsigned, 1>(kMinBufferSize);
      kinfo[eng][i].pibuf = i >= 3? kinfo[eng][i - 3].pibuf: new buffer<char, 1>(input_alloc_size);
      kinfo[eng][i].pobuf =i >= 3 ? kinfo[eng][i - 3].pobuf : new buffer<char, 1>(outputSize);
      kinfo[eng][i].pobuf_decompress = (char *)malloc(kinfo[eng][i].file_size);
    }
  }

  // Create events for the various parts of the execution so that we can profile
  // their performance.
  event e_input_dma     [kNumEngines][buffers_count]; // Input to the GZIP engine. This is a transfer from host to device.
  event e_output_dma    [kNumEngines][buffers_count]; // Output from the GZIP engine. This is transfer from device to host.
  event e_crc_dma       [kNumEngines][buffers_count]; // Transfer CRC from device to host
  event e_size_dma      [kNumEngines][buffers_count]; // Transfer compressed file size from device to host
  event e_k_crc         [kNumEngines][buffers_count]; // CRC kernel
  event e_k_lz          [kNumEngines][buffers_count]; // LZ77 kernel
  event e_k_huff        [kNumEngines][buffers_count]; // Huffman Encoding kernel

#ifndef FPGA_EMULATOR
  dpc_common::TimeInterval perf_timer;
#endif

  
  /*************************************************/
  /* Main loop where the actual execution happens  */
  /*************************************************/
  for (int i = 0; i < buffers_count; i++) {
    for (size_t eng = 0; eng < kNumEngines; eng++) {
      // Transfer the input data, to be compressed, from host to device.
      //将要压缩的输入数据从主机传输到设备
      e_input_dma[eng][i] = q.submit([&](handler &h) {
        auto in_data =kinfo[eng][i].pibuf->get_access<access::mode::discard_write>(h);
        h.copy(kinfo[eng][i].pref_buffer, in_data);
      });

      /************************************/
      /************************************/
      /*         LAUNCH GZIP ENGINE       */
      /************************************/
      /************************************/
      SubmitGzipTasks(q
                      , kinfo[eng][i].file_size
                      , kinfo[eng][i].pibuf
                      ,kinfo[eng][i].pobuf
                      , kinfo[eng][i].gzip_out_buf
                      ,kinfo[eng][i].current_crc
                      , kinfo[eng][i].last_block
                      ,e_k_crc[eng][i]
                      , e_k_lz[eng][i]
                      , e_k_huff[eng][i]
                      , eng);

      // Transfer the output (compressed) data from device to host.

      //将输出(压缩)数据从设备传输到主机
      // Immediate code to set up task graph node
        std::cout << "current_crc=====> " << kinfo[eng][i].current_crc <<std::endl;
      e_output_dma[eng][i] = q.submit([&](handler &h) {
        auto out_data = kinfo[eng][i].pobuf->get_access<access::mode::read>(h);
        h.copy(out_data, kinfo[eng][i].poutput_buffer);
      });

      // Transfer the file size of the compressed output file from device to host.
      //将压缩输出文件的文件大小从设备传输到主机
      e_size_dma[eng][i] = q.submit([&](handler &h) {
        auto out_data =kinfo[eng][i].gzip_out_buf->get_access<access::mode::read>(h);
        h.copy(out_data, kinfo[eng][i].out_info);
      });

      // Transfer the CRC of the compressed output file from device to host.
      //将压缩输出文件的CRC从设备传输到主机
      e_crc_dma[eng][i] = q.submit([&](handler &h) {
        auto out_data = kinfo[eng][i].current_crc->get_access<access::mode::read>(h);
        h.copy(out_data, kinfo[eng][i].buffer_crc);
      });
    }
  }

  // Wait for all kernels to complete
  //等待内核处理
  for (int eng = 0; eng < kNumEngines; eng++) {
    for (int i = 0; i < buffers_count; i++) {
      e_output_dma[eng][i].wait();
      e_size_dma[eng][i].wait();
      e_crc_dma[eng][i].wait();
    }
  }

// Stop the timer.
#ifndef FPGA_EMULATOR
  double diff_total = perf_timer.Elapsed();
  double gbps = iterations * isz / (double)diff_total / 1000000000.0;
#endif

  // Check the compressed file size from each iteration. Make sure the size is actually
  // less-than-or-equal to the input size. Also calculate the remaining CRC.
  size_t compressed_sz[kNumEngines];
  for (int eng = 0; eng < kNumEngines; eng++) {
    compressed_sz[eng] = 0;
    for (int i = 0; i < buffers_count; i++) {
      if (kinfo[eng][i].out_info[0].compression_sz > kinfo[eng][i].file_size) {
        std::cerr << "Unsupported: compressed file larger than input file( "
                  << kinfo[eng][i].out_info[0].compression_sz << " )\n";
        return 1;
      }
      // The majority of the CRC is calculated by the CRC kernel on the FPGA. But the kernel
      // operates on quantized chunks of input data, so any remaining input data, that falls
      // outside the quanta, is included in the overall CRC calculation via the following 
      // function that runs on the host. The last argument is the running CRC that was computed
      // on the FPGA.
      kinfo[eng][i].buffer_crc[0] =Crc32(kinfo[eng][i].pref_buffer, kinfo[eng][i].file_size,kinfo[eng][i].buffer_crc[0]);
      // Accumulate the compressed size across all iterations. Used to 
      // compute compression ratio later.
      compressed_sz[eng] += kinfo[eng][i].out_info[0].compression_sz;
    }
  }

  // delete the file mapping now that all kernels are complete, and we've
  // snapped the time delta
  if (prepin) {
    free(pinbuf, q.get_context());
  } else {
    delete pinbuf;
  }

  // Write the output compressed data from the first iteration of each engine, to a file.
  for (int eng = 0; eng < kNumEngines; eng++) {
    // WriteBlockGzip() returns 1 on failure
    std::cout << "crc======>:"<<kinfo[eng][0].buffer_crc[0]<<"\n";
    if (report && WriteBlockGzip(input_file, outfilenames[eng], kinfo[eng][0].poutput_buffer,
                        kinfo[eng][0].out_info[0].compression_sz,
                        kinfo[eng][0].file_size, kinfo[eng][0].buffer_crc[0])) {
      std::cout << "FAILED\n";
      return 1;
    }        
  }

  // Decompress the output from engine-0 and compare against the input file. Only engine-0's
  // output is verified since all engines are fed the same input data.
  //验证数据
  if (report && CompareGzipFiles(input_file, outfilenames[0])) {
    std::cout << "FAILED\n";
    return 1;
  }

  // Generate throughput report
  // First gather all the execution times.
  size_t time_k_crc[kNumEngines];
  size_t time_k_lz[kNumEngines];
  size_t time_k_huff[kNumEngines];
  size_t time_input_dma[kNumEngines];
  size_t time_output_dma[kNumEngines];
  for (int eng = 0; eng < kNumEngines; eng++) {
    time_k_crc[eng] = 0;
    time_k_lz[eng] = 0;
    time_k_huff[eng] = 0;
    time_input_dma[eng] = 0;
    time_output_dma[eng] = 0;
    for (int i = 0; i < buffers_count; i++) {
      e_k_crc[eng][i].wait();
      e_k_lz[eng][i].wait();
      e_k_huff[eng][i].wait();
      time_k_crc[eng]       += SyclGetExecTimeNs(e_k_crc[eng][i]);
      time_k_lz[eng]        += SyclGetExecTimeNs(e_k_lz[eng][i]);
      time_k_huff[eng]      += SyclGetExecTimeNs(e_k_huff[eng][i]);
      time_input_dma[eng]   += SyclGetExecTimeNs(e_input_dma[eng][i]);
      time_output_dma[eng]  += SyclGetExecTimeNs(e_output_dma[eng][i]);
    }
  }

  if (report) {
    double compression_ratio =((double)compressed_sz[0] / (double)isz / iterations);
#ifndef FPGA_EMULATOR
    std::cout << "Throughput: " << kNumEngines * gbps << " GB/s\n\n";
    for (int eng = 0; eng < kNumEngines; eng++) {
      std::cout << "TP breakdown for engine #" << eng << " (GB/s)\n";
      std::cout << "CRC = " << iterations * isz / (double)time_k_crc[eng]
                << "\n";
      std::cout << "LZ77 = " << iterations * isz / (double)time_k_lz[eng]
                << "\n";
      std::cout << "Huffman Encoding = "
                << iterations * isz / (double)time_k_huff[eng] << "\n";
      std::cout << "DMA host-to-device = "
                << iterations * isz / (double)time_input_dma[eng] << "\n";
      std::cout << "DMA device-to-host = "
                << iterations * isz / (double)time_output_dma[eng] << "\n\n";
    }
#endif
    std::cout << "Compression Ratio " << compression_ratio * 100 << "%\n";
  }

  // Cleanup anything that was allocated by this routine.
  for (int eng = 0; eng < kNumEngines; eng++) {
    for (int i = 0; i < buffers_count; i++) {
      if (i < 3) {
        delete kinfo[eng][i].gzip_out_buf;
        delete kinfo[eng][i].current_crc;
        delete kinfo[eng][i].pibuf;
        delete kinfo[eng][i].pobuf;
        if (prepin) {
          free(kinfo[eng][i].poutput_buffer, q.get_context());
        } else {
          free(kinfo[eng][i].poutput_buffer);
        }
      }
      free(kinfo[eng][i].pobuf_decompress);
    }
    free(kinfo[eng]);
  }

  if (report) std::cout << "PASSED\n";
  return 0;
}
