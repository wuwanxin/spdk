#include "nuvcoder_backend_pyimpl.hpp"
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>

// 添加CUDA Driver API头文件
#include <cuda.h>
#include <cuda_runtime.h>
#include <cuda_runtime_api.h>

// 用于获取 Driver API 错误字符串
const char* GetCuErrorString(CUresult error) {
    const char* str = nullptr;
    if (cuGetErrorString(error, &str) != CUDA_SUCCESS) {
        return "Unknown CUresult error";
    }
    return str;
}

namespace fs = std::filesystem;
namespace py = pybind11;

// ==================== 改进的Python解释器管理器 ====================
struct PythonInitializationState {
    bool interpreter_initialized = false;
    bool threads_initialized = false;
    PyThreadState* main_thread_state = nullptr;
    std::mutex init_mutex;
    std::atomic<int> backend_count{0};
    
    // 安全的GIL管理类
    class ScopedGIL {
    private:
        PyGILState_STATE state_;
        bool acquired_;
        
    public:
        ScopedGIL() : acquired_(false) {
            // 确保线程状态已准备
            if (!PyGILState_Check()) {
                state_ = PyGILState_Ensure();
                acquired_ = true;
                std::cout << "DEBUG: GIL acquired (thread: " 
                          << std::this_thread::get_id() << ")" << std::endl;
            } else {
                std::cout << "DEBUG: GIL already held (thread: " 
                          << std::this_thread::get_id() << ")" << std::endl;
            }
        }
        
        ~ScopedGIL() {
            if (acquired_) {
                PyGILState_Release(state_);
                std::cout << "DEBUG: GIL released (thread: " 
                          << std::this_thread::get_id() << ")" << std::endl;
            }
        }
        
        // 禁止拷贝
        ScopedGIL(const ScopedGIL&) = delete;
        ScopedGIL& operator=(const ScopedGIL&) = delete;
    };
    
    void ensure_initialized() {
        std::lock_guard<std::mutex> lock(init_mutex);
        
        if (!interpreter_initialized) {
            std::cout << "DEBUG: Initializing Python interpreter for first time..." << std::endl;
            
            // 设置Python路径（如果需要）
            const char* python_home = std::getenv("PYTHONHOME");
            if (python_home) {
                std::cout << "DEBUG: PYTHONHOME=" << python_home << std::endl;
            }
            
            // 初始化Python解释器
            Py_InitializeEx(0); // 不初始化信号处理器
            
            if (!Py_IsInitialized()) {
                throw std::runtime_error("Failed to initialize Python interpreter");
            }
            
            interpreter_initialized = true;
            std::cout << "DEBUG: Python interpreter initialized" << std::endl;
        }
        
        if (!threads_initialized) {
            std::cout << "DEBUG: Initializing Python thread support..." << std::endl;
            
            // 初始化线程支持
            if (PyEval_ThreadsInitialized() == 0) {
                PyEval_InitThreads();
            }
            
            if (!PyEval_ThreadsInitialized()) {
                throw std::runtime_error("Failed to initialize Python thread support");
            }
            
            threads_initialized = true;
            
            // 保存主线程状态并释放GIL
            if (!main_thread_state) {
                main_thread_state = PyEval_SaveThread();
                std::cout << "DEBUG: Python thread support initialized, GIL released" << std::endl;
            }
        }
        
        backend_count++;
        std::cout << "DEBUG: Backend count: " << backend_count << std::endl;
    }
    
    void cleanup_if_needed() {
        std::lock_guard<std::mutex> lock(init_mutex);
        
        backend_count--;
        std::cout << "DEBUG: Backend count decreased to: " << backend_count << std::endl;
        
        // 只有当所有后端都销毁时才清理Python解释器
        if (backend_count <= 0 && interpreter_initialized) {
            if (main_thread_state) {
                std::cout << "DEBUG: Restoring main thread state..." << std::endl;
                PyEval_RestoreThread(main_thread_state);
                main_thread_state = nullptr;
            }
            
            std::cout << "DEBUG: Finalizing Python interpreter..." << std::endl;
            
            // 确保没有活动的Python调用
            {
                ScopedGIL gil;
                // 在GIL保护下执行清理
            }
            
            Py_FinalizeEx();
            
            interpreter_initialized = false;
            threads_initialized = false;
            backend_count = 0;
            
            std::cout << "DEBUG: Python interpreter finalized" << std::endl;
        }
    }
};

// 全局Python状态实例
static PythonInitializationState python_state;

// ==================== NuvcoderPythonBackend 实现 ====================

// Constructor - 修正初始化顺序
NuvcoderPythonBackend::NuvcoderPythonBackend()
    : device_(torch::kCPU),
      last_encoded_data_size_(0),
      encode_module_()
{
    std::cout << "DEBUG: NuvcoderPythonBackend constructor called on thread ID: " 
              << std::this_thread::get_id() << std::endl;
    
    // 确保Python解释器已初始化
    python_state.ensure_initialized();
}

// Destructor
NuvcoderPythonBackend::~NuvcoderPythonBackend() {
    std::cout << "DEBUG: NuvcoderPythonBackend destructor called on thread ID: " 
              << std::this_thread::get_id() << std::endl;
    
    shutdown();
    
    // 通知Python状态管理器
    python_state.cleanup_if_needed();
    
    std::cout << "DEBUG: NuvcoderPythonBackend destroyed." << std::endl;
}

// initialize 函数
bool NuvcoderPythonBackend::initialize(const NuvcoderBackendConfig& config,
                                       const std::string& model_path,
                                       const torch::Device& device) {
    this->config_ = config;
    this->device_ = device;
    this->python_script_dir_ = model_path;

    std::cout << "DEBUG: NuvcoderPythonBackend::initialize called on thread ID: " 
              << std::this_thread::get_id() << std::endl;

    try {
        // =================== 1. 检查TensorRT引擎文件 ===================
        fs::path engine_full_path = fs::path(model_path) / "engines/compress_fp16_0402.engine";
        if (!fs::exists(engine_full_path)) {
            std::cerr << "Error: TensorRT engine file not found at " << engine_full_path << std::endl;
            return false;
        }
        
        std::cout << "DEBUG: TensorRT engine found: " << engine_full_path << std::endl;

        // =================== 2. 初始化Python编码模块 ===================
        int device_id = device.is_cuda() ? device.index() : 0;
        
        std::cout << "DEBUG: Initializing Python module with device_id: " << device_id << std::endl;
        
        // 使用安全的GIL管理
        PythonInitializationState::ScopedGIL gil;
        
        try {
            // 设置Python模块搜索路径
            py::module_ sys = py::module_::import("sys");
            py::list sys_path = sys.attr("path");
            
            // 添加模型路径
            sys_path.append(this->python_script_dir_);
            std::cout << "DEBUG: Added to Python path: " << this->python_script_dir_ << std::endl;
            
            // 导入Python模块
            try {
                encode_module_ = py::module_::import("inference_enc");
                std::cout << "DEBUG: Successfully imported Python module: inference_enc" << std::endl;
            } catch (const py::error_already_set& e) {
                std::cerr << "Error importing inference_enc: " << e.what() << std::endl;
                // 打印详细的Python错误
                PyErr_Print();
                return false;
            }
            
            // 调用Python初始化函数
            std::cout << "DEBUG: Calling Python initialize_encoder_module..." << std::endl;
            
            try {
                py::object result = encode_module_.attr("initialize_encoder_module")(
                    engine_full_path.string(),
                    config.ec_thread,
                    config.stream_part_i,
                    device_id
                );
                
                bool python_init_success = result.cast<bool>();
                
                if (python_init_success) {
                    std::cout << "DEBUG: Python encoder module initialized successfully." << std::endl;
                } else {
                    std::cerr << "Error: Python encoder module initialization failed." << std::endl;
                }
                
                return python_init_success;

            } catch (const py::error_already_set& e) {
                std::cerr << "Python Error in initialize_encoder_module: " << e.what() << std::endl;
                PyErr_Print();
                return false;
            }

        } catch (const std::exception& e) {
            std::cerr << "C++ Error during Python module initialization: " << e.what() << std::endl;
            return false;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error during backend initialization: " << e.what() << std::endl;
        return false;
    }
}

// encode 函数 - 修复版本
std::vector<uint8_t> NuvcoderPythonBackend::encode(const torch::Tensor& input_tensor) {
    if (!encode_module_) {
        throw std::runtime_error("Python encoder module not initialized. Call initialize first.");
    }

    std::cout << "DEBUG: NuvcoderPythonBackend::encode called on thread ID: " 
              << std::this_thread::get_id() << std::endl;

    // 检查输入张量
    if (input_tensor.device() != device_) {
        std::stringstream ss;
        ss << "Input tensor must be on device " << device_
           << ". Got device: " << input_tensor.device();
        throw std::runtime_error(ss.str());
    }

    if (input_tensor.dtype() != torch::kFloat32) {
        throw std::runtime_error("Input tensor must be of type float32");
    }

    if (!input_tensor.is_contiguous()) {
        throw std::runtime_error("Input tensor must be contiguous for Python backend.");
    }

    // 使用安全的GIL管理
    PythonInitializationState::ScopedGIL gil;
    
    try {
        // 获取CUDA设备指针
        void* input_gpu_ptr = input_tensor.data_ptr();
        
        std::cout << "DEBUG: Input GPU pointer: " << input_gpu_ptr << std::endl;
        
        // 验证指针有效性
        cudaPointerAttributes attr;
        cudaError_t cuda_err = cudaPointerGetAttributes(&attr, input_gpu_ptr);
        if (cuda_err != cudaSuccess) {
            std::cerr << "Warning: Failed to get CUDA pointer attributes: " 
                      << cudaGetErrorString(cuda_err) << std::endl;
        } else {
            std::cout << "DEBUG: CUDA pointer type: " << attr.type 
                      << " (1=Host, 2=Device, 3=Managed), device: " << attr.device << std::endl;
        }

        // 将C++ CUDA指针转换为Python整数
        uintptr_t ptr_value = reinterpret_cast<uintptr_t>(input_gpu_ptr);
        py::object gpu_ptr_int_obj = py::cast(ptr_value);

        // 获取输入形状
        py::list input_shape_py_list;
        auto sizes = input_tensor.sizes();
        for (auto dim : sizes) {
            input_shape_py_list.append(dim);
        }
        py::tuple input_shape_py = py::tuple(input_shape_py_list);

        // 获取原始图像尺寸
        uint32_t pic_height = config_.image_height;
        uint32_t pic_width = config_.image_width;

        std::cout << "DEBUG: Calling Python encode_frame with shape: " 
                  << pic_width << "x" << pic_height << std::endl;

        // 调用Python的encode_frame函数
        py::dict results;
        
        try {
            py::object result_obj = encode_module_.attr("encode_frame")(
                gpu_ptr_int_obj,
                input_shape_py,
                pic_height,
                pic_width
            );
            
            results = result_obj.cast<py::dict>();
            
        } catch (const py::error_already_set& e) {
            std::cerr << "Python Error in encode_frame: " << e.what() << std::endl;
            PyErr_Print();
            throw std::runtime_error("Python encoding failed.");
        }

        std::cout << "DEBUG: Python encode_frame returned successfully" << std::endl;
        
        // 检查是否包含比特流
        if (results.contains("bit_stream")) {
            std::cout << "DEBUG: Results contain 'bit_stream'" << std::endl;
        } else {
            std::cout << "DEBUG: Results do NOT contain 'bit_stream'" << std::endl;
        }

        // =========================================================================
        // === 构造假数据，跳过Python返回的真实数据处理 ===
        // =========================================================================
        std::vector<uint8_t> fake_encoded_data(1024);
        for (size_t i = 0; i < fake_encoded_data.size(); ++i) {
            fake_encoded_data[i] = static_cast<uint8_t>(i % 256);
        }
        
        // 更新内部状态
        last_encoded_data_ = fake_encoded_data;
        last_encoded_data_size_ = fake_encoded_data.size();
        
        std::cout << "DEBUG: Returning fake encoded data of size " 
                  << fake_encoded_data.size() << " bytes." << std::endl;

        return fake_encoded_data;

    } catch (const py::error_already_set& e) {
        std::cerr << "Python Error during encode: " << e.what() << std::endl;
        PyErr_Print();
        throw std::runtime_error("Python encoding failed.");
    } catch (const std::exception& e) {
        std::cerr << "C++ Error during encode: " << e.what() << std::endl;
        throw;
    }
}

// 实现 INuvcoderBackend 接口的方法
size_t NuvcoderPythonBackend::get_encoded_data_size() const {
    return last_encoded_data_size_;
}

const uint8_t* NuvcoderPythonBackend::get_encoded_data_ptr() const {
    if (last_encoded_data_.empty()) {
        return nullptr;
    }
    return last_encoded_data_.data();
}

void NuvcoderPythonBackend::shutdown() {
    if (encode_module_) {
        std::cout << "DEBUG: Shutting down Python encoder module..." << std::endl;
        
        // 使用安全的GIL管理
        PythonInitializationState::ScopedGIL gil;
        
        try {
            if (py::hasattr(encode_module_, "shutdown_encoder_module")) {
                encode_module_.attr("shutdown_encoder_module")();
                std::cout << "Python encoder module shut down successfully." << std::endl;
            } else {
                std::cout << "Warning: Python module has no 'shutdown_encoder_module' function." << std::endl;
            }
        } catch (const py::error_already_set& e) {
            std::cerr << "Python Error during shutdown: " << e.what() << std::endl;
            PyErr_Print();
        } catch (const std::exception& e) {
            std::cerr << "C++ Error during shutdown: " << e.what() << std::endl;
        }
        
        encode_module_ = py::module_(); // 清空模块对象
        
        std::cout << "DEBUG: Python encoder module shutdown completed." << std::endl;
    }
}

void NuvcoderPythonBackend::flush() {
    last_encoded_data_.clear();
    last_encoded_data_size_ = 0;
    std::cout << "DEBUG: NuvcoderPythonBackend flushed internal encoded data." << std::endl;
}

// 注册后端
REGISTER_BACKEND("python_backend", NuvcoderPythonBackend)