#include <stdlib.h>
#include <iostream>
#include <sstream>

#ifdef linux
#include <linux/limits.h>
#else
#define PATH_MAX 260
#endif

#if defined(_WIN32)
#include <direct.h>
#define getcwd _getcwd
#define realpath(N, R) _fullpath((R), (N), PATH_MAX)
#else
#include <unistd.h>
#endif

#include "NvInfer.h"
#include "third_party/args/args.hpp"
#include "torch/script.h"
#include "torch/torch.h"

#include "torch_tensorrt/logging.h"
#include "torch_tensorrt/ptq.h"
#include "torch_tensorrt/torch_tensorrt.h"

at::ScalarType to_torch_dtype(torchtrt::DataType dtype) {
  switch (dtype) {
    case torchtrt::DataType::kHalf:
      return at::kHalf;
    case torchtrt::DataType::kChar:
      return at::kChar;
    case torchtrt::DataType::kInt:
      return at::kInt;
    case torchtrt::DataType::kBool:
      return at::kBool;
    case torchtrt::DataType::kFloat:
    default:
      return at::kFloat;
  }
}

const std::unordered_map<nvinfer1::DataType, at::ScalarType>& get_trt_at_type_map() {
  static const std::unordered_map<nvinfer1::DataType, at::ScalarType> trt_at_type_map = {
      {nvinfer1::DataType::kFLOAT, at::kFloat},
      {nvinfer1::DataType::kHALF, at::kHalf},
      {nvinfer1::DataType::kINT32, at::kInt},
      {nvinfer1::DataType::kINT8, at::kChar},
      {nvinfer1::DataType::kBOOL, at::kBool},
  };
  return trt_at_type_map;
}

bool checkRtol(const at::Tensor& diff, const std::vector<at::Tensor> inputs, float threshold) {
  double maxValue = 0.0;
  for (auto& tensor : inputs) {
    maxValue = fmax(tensor.abs().max().item<float>(), maxValue);
  }
  torchtrt::logging::log(
      torchtrt::logging::Level::kDEBUG,
      std::string("Max Difference: ") + std::to_string(diff.abs().max().item<float>()));
  torchtrt::logging::log(
      torchtrt::logging::Level::kDEBUG, std::string("Acceptable Threshold: ") + std::to_string(threshold));
  return diff.abs().max().item<float>() <= threshold * maxValue;
}

bool almostEqual(const at::Tensor& a, const at::Tensor& b, float threshold) {
  return checkRtol(a - b, {a, b}, threshold);
}

torchtrt::TensorFormat parseTensorFormat(std::string str) {
  std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });

  if (str == "linear" || str == "nchw" || str == "chw" || str == "contiguous") {
    return torchtrt::TensorFormat::kContiguous;
  } else if (str == "nhwc" || str == "hwc" || str == "channels_last") {
    return torchtrt::TensorFormat::kChannelsLast;
  } else {
    torchtrt::logging::log(
        torchtrt::logging::Level::kERROR,
        "Invalid tensor format, options are [ linear | nchw | chw | contiguous | nhwc | hwc | channels_last ], found: " +
            str);
    return torchtrt::TensorFormat::kUnknown;
  }
}

torchtrt::DataType parseDataType(std::string dtype_str) {
  std::transform(
      dtype_str.begin(), dtype_str.end(), dtype_str.begin(), [](unsigned char c) { return std::tolower(c); });
  if (dtype_str == "float" || dtype_str == "float32" || dtype_str == "f32" || dtype_str == "fp32") {
    return torchtrt::DataType::kFloat;
  } else if (dtype_str == "half" || dtype_str == "float16" || dtype_str == "f16" || dtype_str == "fp16") {
    return torchtrt::DataType::kHalf;
  } else if (dtype_str == "char" || dtype_str == "int8" || dtype_str == "i8") {
    return torchtrt::DataType::kChar;
  } else if (dtype_str == "int" || dtype_str == "int32" || dtype_str == "i32") {
    return torchtrt::DataType::kInt;
  } else if (dtype_str == "bool" || dtype_str == "b") {
    return torchtrt::DataType::kBool;
  } else {
    torchtrt::logging::log(
        torchtrt::logging::Level::kERROR,
        "Invalid precision, options are [ float | float32 | fp32 | f32 | half | float16 | fp16 | f16 | char | int8 | i8 | int | int32 | i32 | bool | b], found: " +
            dtype_str);
    return torchtrt::DataType::kUnknown;
  }
}

std::vector<int64_t> parseSingleDim(std::string shape_str) {
  std::vector<int64_t> shape;
  std::stringstream ss;
  for (auto c : shape_str) {
    if (c == '(' || c == ' ') {
      continue;
    } else if (c == ',') {
      int64_t dim;
      ss >> dim;
      shape.push_back(dim);
      ss.clear();
    } else if (c == ')') {
      int64_t dim;
      ss >> dim;
      shape.push_back(dim);
      ss.clear();
      return shape;
    } else {
      ss << c;
    }
  }

  torchtrt::logging::log(
      torchtrt::logging::Level::kERROR,
      "Shapes need dimensions delimited by comma in parentheses, \"(N,..,C,H,W)\"\n e.g \"(3,3,200,200)\"");
  exit(1);
  return {};
}

std::vector<std::vector<int64_t>> parseDynamicDim(std::string shape_str) {
  shape_str = shape_str.substr(1, shape_str.size() - 2);
  std::vector<std::vector<int64_t>> shape;
  std::stringstream ss;

  std::string delimiter = ";";

  size_t pos = 0;
  while ((pos = shape_str.find(delimiter)) != std::string::npos) {
    auto token = shape_str.substr(0, pos);
    auto range = parseSingleDim(token);
    shape_str.erase(0, pos + delimiter.length());
    shape.push_back(range);
  }

  auto range = parseSingleDim(shape_str);
  shape.push_back(range);

  if (shape.size() != 3) {
    torchtrt::logging::log(
        torchtrt::logging::Level::kERROR,
        "Dynamic shapes need three sets of dimensions delimited by semi-colons, \"[(MIN_N,..,MIN_C,MIN_H,MIN_W);(OPT_N,..,OPT_C,OPT_H,OPT_W);(MAX_N,..,MAX_C,MAX_H,MAX_W)]\"\n e.g \"[(3,3,100,100);(3,3,200,200);(3,3,300,300)]\"");
    exit(1);
  }

  return shape;
}

std::string read_buf(std::string const& path) {
  std::string buf;
  std::ifstream stream(path.c_str(), std::ios::binary);

  if (stream) {
    stream >> std::noskipws;
    std::copy(std::istream_iterator<char>(stream), std::istream_iterator<char>(), std::back_inserter(buf));
  }

  return buf;
}

std::string get_cwd() {
  char buff[FILENAME_MAX]; // create string buffer to hold path
  if (getcwd(buff, FILENAME_MAX)) {
    std::string current_working_dir(buff);
    return current_working_dir;
  } else {
    torchtrt::logging::log(torchtrt::logging::Level::kERROR, "Unable to get current directory");
    exit(1);
  }
}

std::string real_path(std::string path) {
  auto abs_path = path;
  char real_path_c[PATH_MAX];
  char* res = realpath(abs_path.c_str(), real_path_c);
  if (res) {
    return std::string(real_path_c);
  } else {
    torchtrt::logging::log(torchtrt::logging::Level::kERROR, std::string("Unable to find file ") + abs_path);
    exit(1);
  }
}

std::string resolve_path(std::string path) {
  auto rpath = path;
  if (!(rpath.rfind("/", 0) == 0)) {
    rpath = get_cwd() + '/' + rpath;
  }
  return rpath;
}

int main(int argc, char** argv) {
  torchtrt::logging::set_is_colored_output_on(true);
  torchtrt::logging::set_reportable_log_level(torchtrt::logging::Level::kWARNING);
  torchtrt::logging::set_logging_prefix("");

  args::ArgumentParser parser(
      "torchtrtc is a compiler for TorchScript, it will compile and optimize TorchScript programs to run on NVIDIA GPUs using TensorRT",
      "");
  args::HelpFlag help(parser, "help", "Display this help menu", {'h', "help"});

  args::Group group(parser, "Verbiosity of the compiler", args::Group::Validators::AtMostOne);
  args::Flag verbose(
      group, "verbose", "Dumps debugging information about the compilation process onto the console", {'v', "verbose"});
  args::Flag warning(
      group,
      "warning",
      "Disables warnings generated during compilation onto the console (warnings are on by default)",
      {'w', "warnings"});
  args::Flag info(group, "info", "Dumps info messages generated during compilation onto the console", {"i", "info"});

  args::Flag build_debuggable_engine(
      parser, "build-debuggable-engine", "Creates a debuggable engine", {"build-debuggable-engine"});
  args::Flag use_strict_types(
      parser, "use-strict-types", "Restrict operating type to only use set operation precision", {"use-strict-types"});
  args::Flag allow_gpu_fallback(
      parser,
      "allow-gpu-fallback",
      "(Only used when targeting DLA (device-type)) Lets engine run layers on GPU if they are not supported on DLA",
      {"allow-gpu-fallback"});

  args::Flag require_full_compilation(
      parser,
      "require-full-compilation",
      "Require that the model should be fully compiled to TensorRT or throw an error",
      {"require-full-compilation"});

  args::Flag disable_tf32(
      parser, "disable-tf32", "Prevent Float32 layers from using the TF32 data format", {"disable-tf32"});

  args::Flag sparse_weights(
      parser, "sparse-weights", "Enable sparsity for weights of conv and FC layers", {"sparse-weights"});

  args::ValueFlagList<std::string> enabled_precision(
      parser,
      "precision",
      "(Repeatable) Enabling an operating precision for kernels to use when building the engine (Int8 requires a calibration-cache argument) [ float | float32 | f32 | fp32 | half | float16 | f16 | fp16 | int8 | i8 | char ] (default: float)",
      {'p', "enabled-precision"});
  args::ValueFlag<std::string> device_type(
      parser,
      "type",
      "The type of device the engine should be built for [ gpu | dla ] (default: gpu)",
      {'d', "device-type"});
  args::ValueFlag<uint64_t> gpu_id(
      parser, "gpu_id", "GPU id if running on multi-GPU platform (defaults to 0)", {"gpu-id"});
  args::ValueFlag<uint64_t> dla_core(
      parser, "dla_core", "DLACore id if running on available DLA (defaults to 0)", {"dla-core"});

  args::ValueFlag<std::string> engine_capability(
      parser,
      "capability",
      "The type of device the engine should be built for [ standard | safety | dla_standalone ]",
      {"engine-capability"});

  args::ValueFlag<std::string> calibration_cache_file(
      parser,
      "file_path",
      "Path to calibration cache file to use for post training quantization",
      {"calibration-cache-file"});

  args::ValueFlagList<std::string> torch_executed_ops(
      parser,
      "torch-executed-ops",
      "(Repeatable) Operator in the graph that should always be run in PyTorch for execution (partial compilation must be enabled)",
      {"teo", "torch-executed-ops"});

  args::ValueFlagList<std::string> torch_executed_mods(
      parser,
      "torch-executed-mods",
      "(Repeatable) Module that should always be run in Pytorch for execution (partial compilation must be enabled)",
      {"tem", "torch-executed-mods"});

  args::ValueFlag<uint64_t> min_block_size(
      parser,
      "min-block-size",
      "Minimum number of contiguous TensorRT supported ops to compile a subgraph to TensorRT",
      {"mbs", "min-block-size"});

  args::Flag embed_engine(
      parser,
      "embed-engine",
      "Whether to treat input file as a serialized TensorRT engine and embed it into a TorchScript module (device spec must be provided)",
      {"embed-engine"});

  args::ValueFlag<uint64_t> num_min_timing_iters(
      parser, "num_iters", "Number of minimization timing iterations used to select kernels", {"num-min-timing-iter"});
  args::ValueFlag<uint64_t> num_avg_timing_iters(
      parser, "num_iters", "Number of averaging timing iterations used to select kernels", {"num-avg-timing-iters"});
  args::ValueFlag<uint64_t> workspace_size(
      parser, "workspace_size", "Maximum size of workspace given to TensorRT", {"workspace-size"});
  args::ValueFlag<uint64_t> max_batch_size(
      parser, "max_batch_size", "Maximum batch size (must be >= 1 to be set, 0 means not set)", {"max-batch-size"});
  args::ValueFlag<double> threshold(
      parser,
      "threshold",
      "Maximum acceptable numerical deviation from standard torchscript output (default 2e-5)",
      {'t', "threshold"});

  args::Flag no_threshold_check(
      parser, "no-threshold-check", "Skip checking threshold compliance", {"no-threshold-check", "no-threshold-check"});
  args::Flag truncate_long_and_double(
      parser,
      "truncate-long-double",
      "Truncate weights that are provided in 64bit to 32bit (Long, Double to Int, Float)",
      {"truncate", "truncate-long-double", "truncate-64bit"});

  args::Flag save_engine(
      parser,
      "save_engine",
      "Instead of compiling a full a TorchScript program, save the created engine to the path specified as the output path",
      {"save-engine"});
  args::Positional<std::string> input_path(parser, "input_file_path", "Path to input TorchScript file");
  args::Positional<std::string> output_path(
      parser, "output_file_path", "Path for compiled TorchScript (or TensorRT engine) file");
  args::PositionalList<std::string> input_shapes(
      parser,
      "input_specs",
      "Specs for inputs to engine, can either be a single size or a range defined by Min, Optimal, Max sizes, e.g. \"(N,..,C,H,W)\" \"[(MIN_N,..,MIN_C,MIN_H,MIN_W);(OPT_N,..,OPT_C,OPT_H,OPT_W);(MAX_N,..,MAX_C,MAX_H,MAX_W)]\". Data Type and format can be specified by adding an \"@\" followed by dtype and \"%\" followed by format to the end of the shape spec. e.g. \"(3, 3, 32, 32)@f16\%NHWC\"");

  try {
    parser.ParseCLI(argc, argv);
  } catch (args::Help const&) {
    std::cout << parser;
    return 0;
  } catch (args::ParseError const& e) {
    torchtrt::logging::log(torchtrt::logging::Level::kERROR, e.what());
    std::cerr << std::endl << parser;
    return 1;
  }

  if (verbose) {
    torchtrt::logging::set_reportable_log_level(torchtrt::logging::Level::kDEBUG);
  } else if (info) {
    torchtrt::logging::set_reportable_log_level(torchtrt::logging::Level::kINFO);
  } else if (warning) {
    torchtrt::logging::set_reportable_log_level(torchtrt::logging::Level::kERROR);
  }

  std::vector<torchtrt::Input> ranges;
  const std::string spec_err_str =
      "Dimensions should be specified in one of these types \"(N,..,C,H,W)\" \"[(MIN_N,..,MIN_C,MIN_H,MIN_W);(OPT_N,..,OPT_C,OPT_H,OPT_W);(MAX_N,..,MAX_C,MAX_H,MAX_W)]\"\n e.g \"(3,3,300,300)\" \"[(3,3,100,100);(3,3,200,200);(3,3,300,300)]\"\nTo specify input type append an @ followed by the precision\n e.g. \"(3,3,300,300)@f32\"\nTo specify input format append an \% followed by the format [contiguous | channel_last]\n e.g. \"(3,3,300,300)@f32\%channel_last\"";
  for (const auto spec : args::get(input_shapes)) {
    std::string shapes;
    std::string dtype;
    std::string format;
    // THERE IS A SPEC FOR DTYPE
    if (spec.find('@') != std::string::npos) {
      // THERE IS ALSO A SPEC FOR FORMAT
      if (spec.find('%') != std::string::npos) {
        auto dtype_delim = spec.find('@');
        auto format_delim = spec.find('%');
        std::string shapes = spec.substr(0, dtype_delim);
        std::string dtype = spec.substr(dtype_delim + 1, format_delim - (dtype_delim + 1));
        std::string format = spec.substr(format_delim + 1, spec.size());

        auto parsed_dtype = parseDataType(dtype);
        if (parsed_dtype == torchtrt::DataType::kUnknown) {
          torchtrt::logging::log(torchtrt::logging::Level::kERROR, "Invalid datatype for input specification " + spec);
          std::cerr << std::endl << parser;
          exit(1);
        }
        auto parsed_format = parseTensorFormat(format);
        if (parsed_format == torchtrt::TensorFormat::kUnknown) {
          torchtrt::logging::log(torchtrt::logging::Level::kERROR, "Invalid format for input specification " + spec);
          std::cerr << std::endl << parser;
          exit(1);
        }
        if (shapes.rfind("(", 0) == 0) {
          ranges.push_back(torchtrt::Input(parseSingleDim(shapes), parsed_dtype, parsed_format));
        } else if (shapes.rfind("[", 0) == 0) {
          auto dyn_shapes = parseDynamicDim(shapes);
          ranges.push_back(torchtrt::Input(dyn_shapes[0], dyn_shapes[1], dyn_shapes[2], parsed_dtype, parsed_format));
        } else {
          torchtrt::logging::log(torchtrt::logging::Level::kERROR, spec_err_str);
          std::cerr << std::endl << parser;
          exit(1);
        }
        // THERE IS NO SPEC FOR FORMAT
      } else {
        std::string shapes = spec.substr(0, spec.find('@'));
        std::string dtype = spec.substr(spec.find('@') + 1, spec.size());

        auto parsed_dtype = parseDataType(dtype);
        if (parsed_dtype == torchtrt::DataType::kUnknown) {
          torchtrt::logging::log(torchtrt::logging::Level::kERROR, "Invalid datatype for input specification " + spec);
          std::cerr << std::endl << parser;
          exit(1);
        }
        if (shapes.rfind("(", 0) == 0) {
          ranges.push_back(torchtrt::Input(parseSingleDim(shapes), parsed_dtype));
        } else if (shapes.rfind("[", 0) == 0) {
          auto dyn_shapes = parseDynamicDim(shapes);
          ranges.push_back(torchtrt::Input(dyn_shapes[0], dyn_shapes[1], dyn_shapes[2], parsed_dtype));
        } else {
          torchtrt::logging::log(torchtrt::logging::Level::kERROR, spec_err_str);
          std::cerr << std::endl << parser;
          exit(1);
        }
      }
      // THERE IS A SPEC FOR FORMAT BUT NOT DTYPE
    } else if (spec.find('%') != std::string::npos) {
      std::string shapes = spec.substr(0, spec.find('%'));
      std::string format = spec.substr(spec.find('%') + 1, spec.size());

      auto parsed_format = parseTensorFormat(format);
      if (parsed_format == torchtrt::TensorFormat::kUnknown) {
        torchtrt::logging::log(torchtrt::logging::Level::kERROR, "Invalid format for input specification " + spec);
        std::cerr << std::endl << parser;
        exit(1);
      }
      if (shapes.rfind("(", 0) == 0) {
        ranges.push_back(torchtrt::Input(parseSingleDim(shapes), parsed_format));
      } else if (shapes.rfind("[", 0) == 0) {
        auto dyn_shapes = parseDynamicDim(shapes);
        ranges.push_back(torchtrt::Input(dyn_shapes[0], dyn_shapes[1], dyn_shapes[2], parsed_format));
      } else {
        torchtrt::logging::log(torchtrt::logging::Level::kERROR, spec_err_str);
        std::cerr << std::endl << parser;
        exit(1);
      }
      // JUST SHAPE USE DEFAULT DTYPE
    } else {
      if (spec.rfind("(", 0) == 0) {
        ranges.push_back(torchtrt::Input(parseSingleDim(spec)));
      } else if (spec.rfind("[", 0) == 0) {
        auto dyn_shapes = parseDynamicDim(spec);
        ranges.push_back(torchtrt::Input(dyn_shapes[0], dyn_shapes[1], dyn_shapes[2]));
      } else {
        torchtrt::logging::log(torchtrt::logging::Level::kERROR, spec_err_str);
        std::cerr << std::endl << parser;
        exit(1);
      }
    }
    std::stringstream ss;
    ss << "Parsed Input: " << ranges.back();
    torchtrt::logging::log(torchtrt::logging::Level::kDEBUG, ss.str());
  }

  auto compile_settings = torchtrt::ts::CompileSpec(ranges);

  if (build_debuggable_engine) {
    compile_settings.debug = true;
  }

  if (use_strict_types) {
    compile_settings.strict_types = true;
  }

  if (allow_gpu_fallback) {
    compile_settings.device.allow_gpu_fallback = true;
  }

  if (disable_tf32) {
    compile_settings.disable_tf32 = true;
  }

  if (sparse_weights) {
    compile_settings.sparse_weights = true;
  }

  std::string calibration_cache_file_path = "";
  if (calibration_cache_file) {
    calibration_cache_file_path = resolve_path(args::get(calibration_cache_file));
  }

  auto calibrator = torchtrt::ptq::make_int8_cache_calibrator(calibration_cache_file_path);

  compile_settings.require_full_compilation = require_full_compilation;

  if (torch_executed_ops || torch_executed_mods) {
    if (require_full_compilation) {
      torchtrt::logging::log(
          torchtrt::logging::Level::kERROR,
          "Ops or modules to run in torch were provided but full compilation was requested. Please remove --require-full-compilation to run specified ops and modules in torch.");
      exit(1);
    }

    compile_settings.min_block_size = min_block_size;

    for (const auto _op : args::get(torch_executed_ops)) {
      compile_settings.torch_executed_ops.push_back(_op);
    }

    for (const auto _mod : args::get(torch_executed_mods)) {
      compile_settings.torch_executed_modules.push_back(_mod);
    }
  }

  if (enabled_precision) {
    for (const auto precision : args::get(enabled_precision)) {
      auto dtype = parseDataType(precision);
      if (dtype == torchtrt::DataType::kFloat) {
        compile_settings.enabled_precisions.insert(torch::kF32);
      } else if (dtype == torchtrt::DataType::kHalf) {
        compile_settings.enabled_precisions.insert(torch::kF16);
      } else if (dtype == torchtrt::DataType::kChar) {
        compile_settings.enabled_precisions.insert(torch::kI8);
        if (calibration_cache_file) {
          compile_settings.ptq_calibrator = calibrator;
        } else {
          torchtrt::logging::log(
              torchtrt::logging::Level::kINFO,
              "Int8 precision has been enabled but no calibrator provided. This assumes the network has Q/DQ nodes obtained from Quantization aware training. For more details, refer to https://docs.nvidia.com/deeplearning/tensorrt/developer-guide/index.html#work-with-qat-networks");
        }
      } else {
        std::stringstream ss;
        ss << "Invalid precision given for enabled kernel precision, options are [ float | float32 | f32 | fp32 | half | float16 | f16 | fp16 | char | int8 | i8 ], found: ";
        ss << dtype;
        torchtrt::logging::log(torchtrt::logging::Level::kERROR, ss.str());
        std::cerr << std::endl << parser;
        return 1;
      }
    }
  }

  if (device_type) {
    auto device = args::get(device_type);
    std::transform(device.begin(), device.end(), device.begin(), [](unsigned char c) { return std::tolower(c); });

    if (gpu_id) {
      compile_settings.device.gpu_id = args::get(gpu_id);
      torchtrt::set_device(compile_settings.device.gpu_id);
    }

    if (device == "gpu") {
      compile_settings.device.device_type = torchtrt::Device::DeviceType::kGPU;
    } else if (device == "dla") {
      compile_settings.device.device_type = torchtrt::Device::DeviceType::kDLA;
      if (dla_core) {
        compile_settings.device.dla_core = args::get(dla_core);
      }
    } else {
      torchtrt::logging::log(
          torchtrt::logging::Level::kERROR, "Invalid device type, options are [ gpu | dla ] found: " + device);
      std::cerr << std::endl << parser;
      return 1;
    }
  }

  if (engine_capability) {
    auto capability = args::get(engine_capability);
    std::transform(
        capability.begin(), capability.end(), capability.begin(), [](unsigned char c) { return std::tolower(c); });
    if (capability == "standard") {
      compile_settings.capability = torchtrt::EngineCapability::kSTANDARD;
    } else if (capability == "safety") {
      compile_settings.capability = torchtrt::EngineCapability::kSAFETY;
    } else if (capability == "dla_standalone") {
      compile_settings.capability = torchtrt::EngineCapability::kDLA_STANDALONE;
    } else {
      torchtrt::logging::log(
          torchtrt::logging::Level::kERROR,
          "Invalid engine capability, options are [ standard | safety | dla_standalone ]");
      std::cerr << std::endl << parser;
      return 1;
    }
  }

  if (num_min_timing_iters) {
    compile_settings.num_min_timing_iters = args::get(num_min_timing_iters);
  }

  if (num_avg_timing_iters) {
    compile_settings.num_avg_timing_iters = args::get(num_avg_timing_iters);
  }

  if (workspace_size) {
    compile_settings.workspace_size = args::get(workspace_size);
  }

  if (max_batch_size) {
    compile_settings.max_batch_size = args::get(max_batch_size);
  }

  if (truncate_long_and_double) {
    compile_settings.truncate_long_and_double = true;
  }

  auto real_input_path = resolve_path(args::get(input_path));
  auto real_output_path = resolve_path(args::get(output_path));

  // Instead of compiling, just embed engine in a PyTorch module
  if (embed_engine) {
    std::string serialized_engine = read_buf(real_input_path);
    auto trt_mod = torchtrt::ts::embed_engine_in_new_module(serialized_engine, compile_settings.device);
    trt_mod.save(real_output_path);
    return 0;
  }

  torch::jit::Module mod;
  try {
    // Deserialize the ScriptModule from a file using torch::jit::load().
    mod = torch::jit::load(real_input_path);
  } catch (const c10::Error& e) {
    torchtrt::logging::log(torchtrt::logging::Level::kERROR, "Error loading the model (path may be incorrect)");
    return 1;
  }

  if (require_full_compilation) {
    if (!torchtrt::ts::check_method_operator_support(mod, "forward")) {
      torchtrt::logging::log(torchtrt::logging::Level::kERROR, "Module is not currently supported by Torch-TensorRT");
      return 1;
    }
  }

  if (save_engine) {
    auto engine = torchtrt::ts::convert_method_to_trt_engine(mod, "forward", compile_settings);
    std::ofstream out(real_output_path);
    out << engine;
    out.close();
    return 0;
  } else {
    auto trt_mod = torchtrt::ts::compile(mod, compile_settings);

    if (!no_threshold_check &&
        (compile_settings.enabled_precisions.size() == 1 &&
         compile_settings.enabled_precisions.find(torchtrt::DataType::kFloat) !=
             compile_settings.enabled_precisions.end())) {
      double threshold_val = 2e-5;
      if (threshold) {
        threshold_val = args::get(threshold);
      }

      std::vector<torch::jit::IValue> jit_inputs_ivalues;
      std::vector<torch::jit::IValue> trt_inputs_ivalues;

      for (auto i : ranges) {
        auto in = at::randn(i.opt_shape, {at::kCUDA});
        in = in.to(to_torch_dtype(i.dtype));
        jit_inputs_ivalues.push_back(in.clone());
        trt_inputs_ivalues.push_back(in.clone());
      }

      mod.to({at::kCUDA});
      torch::jit::IValue jit_results_ivalues = mod.forward(jit_inputs_ivalues);
      std::vector<at::Tensor> jit_results;
      if (jit_results_ivalues.isTensor()) {
        jit_results.push_back(jit_results_ivalues.toTensor());
      } else {
        auto results = jit_results_ivalues.toTuple()->elements();
        for (auto r : results) {
          jit_results.push_back(r.toTensor());
        }
      }

      torch::jit::IValue trt_results_ivalues = trt_mod.forward(trt_inputs_ivalues);
      std::vector<at::Tensor> trt_results;
      if (trt_results_ivalues.isTensor()) {
        trt_results.push_back(trt_results_ivalues.toTensor());
      } else {
        auto results = trt_results_ivalues.toTuple()->elements();
        for (auto r : results) {
          trt_results.push_back(r.toTensor());
        }
      }

      for (size_t i = 0; i < trt_results.size(); i++) {
        if (!almostEqual(jit_results[i], trt_results[i].reshape_as(jit_results[i]), threshold_val)) {
          std::ostringstream threshold_ss;
          threshold_ss << threshold_val;
          torchtrt::logging::log(
              torchtrt::logging::Level::kWARNING,
              std::string("Maximum numerical deviation for output exceeds set threshold (") + threshold_ss.str() +
                  std::string(")"));
        }
      }
    } else {
      if (no_threshold_check) {
        torchtrt::logging::log(
            torchtrt::logging::Level::kWARNING, "Threshold check skipped, numerical precision is not checked");
      } else {
        torchtrt::logging::log(
            torchtrt::logging::Level::kWARNING,
            "Due to change in operating data type, numerical precision is not checked");
      }
    }

    trt_mod.save(real_output_path);
  }

  return 0;
}
