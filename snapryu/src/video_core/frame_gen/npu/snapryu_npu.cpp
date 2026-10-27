// SPDX-License-Identifier: GPL-3.0-or-later
// ABI1 retains the legacy comparison; PackedAnvil is the active Cemu GPU backend.
#include "snapryu_npu.h"

#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include "prepare_cpu.h"
#include "quality_capture.h"
#include "packed_anvil.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "common/logging.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static_assert(sizeof(void*) == 8, "SnapRYU NPU ABI 1 requires a 64-bit host");
static_assert(sizeof(SnapRyuNpuConfig) == 40);
static_assert(sizeof(SnapRyuNpuImage) == 32);
static_assert(sizeof(SnapRyuNpuPair) == 104);
static_assert(sizeof(SnapRyuNpuOutput) == 88);
static_assert(sizeof(SnapRyuNpuStats) == 464);

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr size_t Pixels = size_t{SNAPRYU_NPU_WIDTH} * SNAPRYU_NPU_HEIGHT;
constexpr size_t RgbBytes = Pixels * 3;
constexpr size_t RgbaBytes = Pixels * 4;

int64_t NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

double Ms(Clock::time_point start) noexcept {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// ORT's manual API pointer is process-global within this binary. Pin a single
// verified runtime for its lifetime; never unload it below surviving Ort objects.
// This also prevents two per-game contexts racing to install different APIs.
struct Runtime {
    std::mutex mutex;
    fs::path directory;
    HMODULE rpc{};
    HMODULE ort{};
    DLL_DIRECTORY_COOKIE dll_directory{};
};

Runtime& SharedRuntime() {
    static Runtime runtime;
    return runtime;
}

fs::path LoadedPath(HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        throw std::runtime_error("Cannot resolve loaded DLL path");
    }
    return fs::path{std::wstring{path.data(), size}};
}

void InitializeRuntime(const fs::path& requested) {
    auto& shared = SharedRuntime();
    std::scoped_lock lock{shared.mutex};
    const fs::path directory = fs::canonical(requested);
    if (shared.ort) {
        if (!fs::equivalent(shared.directory, directory)) {
            throw std::runtime_error("A different NPU runtime is already loaded in this process");
        }
        return;
    }
    const std::array required{L"onnxruntime.dll", L"onnxruntime_providers_qnn.dll",
                              L"onnxruntime_providers_shared.dll", L"QnnHtp.dll", L"QnnSystem.dll"};
    for (const auto* name : required) {
        if (!fs::is_regular_file(directory / name)) {
            throw std::runtime_error("NPU runtime is incomplete; check the packaged DLL manifest");
        }
    }
    std::wifstream rpc_file{directory / L"rpc-path.txt"};
    std::wstring rpc_text;
    std::getline(rpc_file, rpc_text);
    if (!rpc_text.empty() && rpc_text.back() == L'\r') {
        rpc_text.pop_back();
    }
    const fs::path rpc_path{rpc_text};
    if (!rpc_file || !rpc_path.is_absolute() || rpc_path.filename() != L"libcdsprpc.dll" ||
        !fs::is_regular_file(rpc_path)) {
        throw std::runtime_error("rpc-path.txt must name the installed driver's libcdsprpc.dll");
    }
    constexpr DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                            LOAD_LIBRARY_SEARCH_USER_DIRS;
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(directory.c_str());
    if (!cookie) {
        throw std::runtime_error("AddDllDirectory for NPU runtime failed");
    }
    HMODULE rpc = LoadLibraryExW(rpc_path.c_str(), nullptr, flags);
    HMODULE ort = nullptr;
    try {
        if (!rpc || !fs::equivalent(LoadedPath(rpc), rpc_path)) {
            throw std::runtime_error(
                "Driver RPC library could not be loaded from the verified path");
        }
        ort = LoadLibraryExW((directory / L"onnxruntime.dll").c_str(), nullptr, flags);
        if (!ort || !fs::equivalent(LoadedPath(ort), directory / L"onnxruntime.dll")) {
            throw std::runtime_error("ORT library could not be loaded from the verified path");
        }
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        const auto get_api = reinterpret_cast<GetApiBase>(GetProcAddress(ort, "OrtGetApiBase"));
        if (!get_api) {
            throw std::runtime_error("OrtGetApiBase export missing");
        }
        const auto* base = get_api();
        if (!base || std::string{base->GetVersionString()} != "1.24.4") {
            throw std::runtime_error("This engine requires the validated ORT 1.24.4 runtime");
        }
        const auto* api = base->GetApi(ORT_API_VERSION);
        if (!api) {
            throw std::runtime_error("ORT C API version mismatch");
        }
        Ort::InitApi(api);
        shared.directory = directory;
        shared.rpc = rpc;
        shared.ort = ort;
        shared.dll_directory = cookie;
    } catch (...) {
        if (ort) {
            FreeLibrary(ort);
        }
        if (rpc) {
            FreeLibrary(rpc);
        }
        RemoveDllDirectory(cookie);
        throw;
    }
}

class Model {
  public:
    Model(Ort::Env& env, const fs::path& path, const Ort::SessionOptions& options)
        : session{env, path.c_str(), options}, allocator{session, memory} {
        Ort::AllocatorWithDefaultOptions names;
        for (const bool input : {true, false}) {
            const size_t count = input ? session.GetInputCount() : session.GetOutputCount();
            for (size_t i = 0; i < count; ++i) {
                const auto name = input ? session.GetInputNameAllocated(i, names)
                                        : session.GetOutputNameAllocated(i, names);
                const std::string key{name.get()};
                const auto info =
                    input ? session.GetInputTypeInfo(i) : session.GetOutputTypeInfo(i);
                const auto tensor = info.GetTensorTypeAndShapeInfo();
                const auto shape = tensor.GetShape();
                if (tensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8 ||
                    std::any_of(shape.begin(), shape.end(), [](int64_t n) { return n <= 0; })) {
                    throw std::runtime_error("Unexpected NPU model tensor contract");
                }
                const size_t bytes = tensor.GetElementCount();
                allocations.emplace_back(allocator.GetAllocation(bytes));
                void* pointer = allocations.back().get();
                pointers.emplace(key, pointer);
                sizes.emplace(key, bytes);
                (input ? input_names : output_names).push_back(key);
                (input ? inputs : outputs)
                    .push_back(Ort::Value::CreateTensor(memory, pointer, bytes, shape.data(),
                                                        shape.size(),
                                                        ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8));
            }
        }
        for (const auto& name : input_names) {
            input_c.push_back(name.c_str());
        }
        for (const auto& name : output_names) {
            output_c.push_back(name.c_str());
        }
    }

    uint8_t* Data(const std::string& name, size_t expected_size) {
        if (sizes.at(name) != expected_size) {
            throw std::runtime_error("NPU model tensor size mismatch");
        }
        return static_cast<uint8_t*>(pointers.at(name));
    }

    void Run() {
        session.Run(Ort::RunOptions{}, input_c.data(), inputs.data(), inputs.size(),
                    output_c.data(), outputs.data(), outputs.size());
        for (size_t i = 0; i < outputs.size(); ++i) {
            if (outputs[i].GetTensorRawData() != pointers.at(output_names[i])) {
                throw std::runtime_error("QNN replaced a caller-owned output tensor");
            }
        }
    }

    void ValidateAnvil(uint32_t height) {
        const uint32_t width=height==720?1280:1920;
        if ((height!=720&&height!=1080)||session.GetInputCount()!=1||session.GetOutputCount()!=1||
            input_names!=std::vector<std::string>{"input"}||output_names!=std::vector<std::string>{"output"}) {
            throw std::runtime_error("Cemu Anvil tensor count/name mismatch");
        }
        const auto input=session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const auto output=session.GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if(input!=std::vector<int64_t>{1,height,width,6}||output!=std::vector<int64_t>{1,height,width,3}) {
            throw std::runtime_error("Cemu Anvil exact NHWC profile mismatch");
        }
    }

  private:
    Ort::Session session;
    Ort::MemoryInfo memory{"QnnHtpShared", OrtDeviceAllocator, 0, OrtMemTypeDefault};
    Ort::Allocator allocator;
    std::vector<Ort::MemoryAllocation> allocations;
    std::vector<std::string> input_names, output_names;
    std::vector<const char*> input_c, output_c;
    std::vector<Ort::Value> inputs, outputs;
    std::map<std::string, void*> pointers;
    std::map<std::string, size_t> sizes;
};

struct Job {
    uint64_t epoch{}, previous{}, current{};
    int64_t deadline{};
    std::vector<uint8_t> a, b;
};

struct Completion {
    SnapRyuNpuOutput info{};
    std::vector<uint8_t> rgba;
};

struct Slot {
    Job input;
    Completion output;
};

struct Engine {
    Engine(const fs::path& runtime, const fs::path& motion_path, const fs::path& anvil_path) {
        InitializeRuntime(runtime);
        env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "snapryu-npu");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
        options.AppendExecutionProvider("QNN", {{"backend_path", "QnnHtp.dll"},
                                                {"offload_graph_io_quantization", "0"},
                                                {"enable_htp_fp16_precision", "1"},
                                                {"enable_htp_shared_memory_allocator", "1"},
                                                {"htp_performance_mode", "balanced"},
                                                {"profiling_level", "off"}});
        motion = std::make_unique<Model>(*env, motion_path, options);
        anvil = std::make_unique<Model>(*env, anvil_path, options);
        a = motion->Data("a", RgbBytes);
        b = motion->Data("b", RgbBytes);
        flow = motion->Data("flow", EdenAnvilCpu::Blocks * 4);
        cost = motion->Data("cost", EdenAnvilCpu::Blocks * 2);
        packed = anvil->Data("input", RgbBytes * 2);
        residual = anvil->Data("output", RgbBytes);
        valid.resize(Pixels);
    }

    void Run(const Job& job, Completion& result) {
        const auto start = Clock::now();
        result.info = {};
        result.info.struct_size = sizeof(result.info);
        result.info.epoch = job.epoch;
        result.info.previous_id = job.previous;
        result.info.current_id = job.current;
        result.info.deadline_ns = job.deadline;
        std::memcpy(a, job.a.data(), RgbBytes);
        std::memcpy(b, job.b.data(), RgbBytes);
        auto mark = Clock::now();
        motion->Run();
        result.info.motion_ms = Ms(mark);
        mark = Clock::now();
#if SNAPRYU_DIAGNOSTICS
        auto& quality = SnapRyuQuality::Capture::Get();
        const bool quality_selected = job.previous && quality.Selected(job.epoch, job.previous, job.current);
        EdenAnvilCpu::PreparationDebug preparation_debug;
#endif
        const auto preparation =
            EdenAnvilCpu::PrepareWarp<true>(a, b, flow, cost, packed, nullptr, valid.data()
#if SNAPRYU_DIAGNOSTICS
                , quality_selected ? &preparation_debug : nullptr
#endif
            );
        result.info.prepare_ms = Ms(mark);
        result.info.scene_cut = preparation.scene_cut;
        result.info.valid_pixels = preparation.valid_pixels;
        mark = Clock::now();
        // The public output still contains B for every uncertain pixel. With
        // no valid pixels the residual cannot contribute to that image.
        // Internal warmup has previous=0 and always runs both actual models.
        if (!job.previous || (!preparation.scene_cut && preparation.valid_pixels != 0)) {
            anvil->Run();
            result.info.anvil_ms = Ms(mark);
        }
        mark = Clock::now();
        result.rgba.resize(RgbaBytes);
        table.RunRgba(packed, residual, b, valid.data(), result.rgba.data());
        result.info.compose_ms = Ms(mark);
        result.info.job_ms = Ms(start);
#if SNAPRYU_DIAGNOSTICS
        if (quality_selected) {
            try {
            const auto add = [&](const char* name,const void* bytes,size_t size,uint32_t channels=0,
                                 const std::string& extra="{}") {
                quality.Add(job.epoch,job.previous,job.current,name,bytes,size,
                    channels ? 1280 : 0,channels ? 720 : 0,channels ? 1280*channels : 0,extra);
            };
            add("input_a_rgb",a,RgbBytes,3,"{\"format\":\"RGB24\",\"origin\":\"completed GPU downscale\"}");
            add("input_b_rgb",b,RgbBytes,3,"{\"format\":\"RGB24\",\"origin\":\"completed GPU downscale\"}");
            add("model_flow_u8",flow,EdenAnvilCpu::Blocks*4);
            add("model_cost_u8",cost,EdenAnvilCpu::Blocks*2);
            add("refined_flow_i32",preparation_debug.refined.data(),sizeof(preparation_debug.refined));
            add("match_scores_u32",preparation_debug.scores.data(),sizeof(preparation_debug.scores));
            add("accepted_u8",preparation_debug.accepted.data(),sizeof(preparation_debug.accepted));
            add("valid_u8",valid.data(),Pixels,1);
            add("warped_packed_rgb6",packed,RgbBytes*2,6);
            if (result.info.anvil_ms > 0.0) add("anvil_residual_u8",residual,RgbBytes,3);
            add("cpu_output_rgba",result.rgba.data(),RgbaBytes,4,
                "{\"format\":\"RGBA8\",\"scene_cut\":"+std::to_string(result.info.scene_cut)+
                ",\"valid_pixels\":"+std::to_string(result.info.valid_pixels)+
                ",\"match_error\":"+std::to_string(preparation.match_error)+
                ",\"residual_available\":"+(result.info.anvil_ms>0.0?"true":"false")+
                ",\"deadline_ns\":"+std::to_string(job.deadline)+
                ",\"job_ms\":"+std::to_string(result.info.job_ms)+
                ",\"motion_ms\":"+std::to_string(result.info.motion_ms)+
                ",\"prepare_ms\":"+std::to_string(result.info.prepare_ms)+
                ",\"anvil_ms\":"+std::to_string(result.info.anvil_ms)+
                ",\"compose_ms\":"+std::to_string(result.info.compose_ms)+"}");
            } catch (...) {
                // Optional diagnostics must not turn a completed NPU job into FAILED.
            }
        }
        const auto content_now = Clock::now();
        if (job.previous && std::chrono::duration<double>(content_now - last_content_report).count() >= 5.0) {
            // Sparse diagnostic on completed CPU pixels, with no readback or new
            // synchronization. A match on a still scene is expected; this is not
            // an image-quality score or a physical-display measurement.
            unsigned samples = 0, source_changed = 0, equal_a = 0, equal_b = 0;
            for (size_t y = 16; y < SNAPRYU_NPU_HEIGHT; y += 32) {
                for (size_t x = 16; x < SNAPRYU_NPU_WIDTH; x += 32) {
                    const size_t p = (y * SNAPRYU_NPU_WIDTH + x) * 3;
                    ++samples;
                    source_changed += std::memcmp(a + p, b + p, 3) != 0;
                    equal_a += std::memcmp(result.rgba.data() + (p / 3) * 4, a + p, 3) == 0;
                    equal_b += std::memcmp(result.rgba.data() + (p / 3) * 4, b + p, 3) == 0;
                }
            }
            LOG_INFO(Render_Vulkan, "SNAPRYU_NPU_CONTENT epoch={} previous={} current={} "
                "valid_pct={:.2f} scene_cut={} sample_pixels={} source_changed_pct={:.2f} "
                "output_equal_previous_pct={:.2f} output_equal_current_pct={:.2f} "
                "job_ms={:.3f} motion_ms={:.3f} prepare_ms={:.3f} anvil_ms={:.3f} compose_ms={:.3f}",
                job.epoch, job.previous, job.current, result.info.valid_pixels * 100.0 / Pixels,
                result.info.scene_cut, samples, source_changed * 100.0 / samples,
                equal_a * 100.0 / samples, equal_b * 100.0 / samples,
                result.info.job_ms, result.info.motion_ms, result.info.prepare_ms,
                result.info.anvil_ms, result.info.compose_ms);
            last_content_report = content_now;
        }
#endif
    }

#if SNAPRYU_DIAGNOSTICS
    Clock::time_point last_content_report{};
#endif
    // Declared in dependency order; reverse destruction destroys tensors before
    // sessions, sessions before environment. The process runtime remains pinned.
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Model> motion, anvil;
    uint8_t *a{}, *b{}, *flow{}, *cost{}, *packed{}, *residual{};
    std::vector<uint8_t> valid;
    EdenAnvilCpu::ComposeTable table;
};

bool ValidImage(const SnapRyuNpuImage& image) noexcept {
    if (!image.pixels || image.width != SNAPRYU_NPU_WIDTH || image.height != SNAPRYU_NPU_HEIGHT ||
        image.format > SNAPRYU_NPU_BGRA8) {
        return false;
    }
    const uint64_t packed_row = uint64_t{image.width} * (image.format == SNAPRYU_NPU_RGB24 ? 3 : 4);
    return image.row_bytes >= packed_row &&
           image.data_bytes >= uint64_t{image.row_bytes} * (image.height - 1) + packed_row;
}

void CopyRgb(const SnapRyuNpuImage& image, std::vector<uint8_t>& rgb) {
    rgb.resize(RgbBytes);
    for (size_t y = 0; y < SNAPRYU_NPU_HEIGHT; ++y) {
        const uint8_t* source = image.pixels + y * image.row_bytes;
        uint8_t* destination = rgb.data() + y * SNAPRYU_NPU_WIDTH * 3;
        if (image.format == SNAPRYU_NPU_RGB24) {
            std::memcpy(destination, source, SNAPRYU_NPU_WIDTH * 3);
        } else {
            const bool bgra = image.format == SNAPRYU_NPU_BGRA8;
            for (size_t x = 0; x < SNAPRYU_NPU_WIDTH; ++x) {
                destination[x * 3 + 0] = source[x * 4 + (bgra ? 2 : 0)];
                destination[x * 3 + 1] = source[x * 4 + 1];
                destination[x * 3 + 2] = source[x * 4 + (bgra ? 0 : 2)];
            }
        }
    }
}
} // namespace

namespace SnapRyuCemu {
struct PackedAnvil::Impl {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Model> model;
    uint8_t* input{};uint8_t* output{};size_t input_bytes{},output_bytes{};
    Impl(const fs::path& runtime,const fs::path& path,uint32_t height) {
        if(height!=720&&height!=1080)throw std::invalid_argument("Cemu processing profile invalid");
        InitializeRuntime(runtime);
        env=std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING,"snapryu-cemu-anvil");
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(1);options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        options.AddConfigEntry("session.disable_cpu_ep_fallback","1");
        options.AppendExecutionProvider("QNN",{{"backend_path","QnnHtp.dll"},
            {"offload_graph_io_quantization","0"},{"enable_htp_fp16_precision","1"},
            {"enable_htp_shared_memory_allocator","1"},{"htp_performance_mode","balanced"},
            {"profiling_level","off"}});
        model=std::make_unique<Model>(*env,path,options);model->ValidateAnvil(height);
        const size_t pixels=size_t(height)*(height==720?1280:1920);
        input_bytes=pixels*6;output_bytes=pixels*3;
        input=model->Data("input",input_bytes);output=model->Data("output",output_bytes);
    }
};
PackedAnvil::PackedAnvil(const std::filesystem::path& runtime,const std::filesystem::path& model,uint32_t height)
    :impl{std::make_unique<Impl>(runtime,model,height)}{}
PackedAnvil::~PackedAnvil()=default;
std::span<const uint8_t> PackedAnvil::InputTensor()const{return {impl->input,impl->input_bytes};}
std::span<const uint8_t> PackedAnvil::ResidualTensor()const{return {impl->output,impl->output_bytes};}
AnvilTiming PackedAnvil::Run(std::span<const uint8_t> packed,std::span<uint8_t> residual) {
    if(packed.size()!=impl->input_bytes||residual.size()!=impl->output_bytes)
        throw std::invalid_argument("Cemu packed/residual size mismatch");
    AnvilTiming timing;const auto start=Clock::now();auto mark=start;
    std::memcpy(impl->input,packed.data(),packed.size());timing.input_copy_ms=Ms(mark);
    mark=Clock::now();impl->model->Run();timing.htp_ms=Ms(mark);
    mark=Clock::now();std::memcpy(residual.data(),impl->output,residual.size());timing.residual_copy_ms=Ms(mark);
    timing.service_ms=Ms(start);return timing;
}
} // namespace SnapRyuCemu

struct SnapRyuNpu {
    explicit SnapRyuNpu(const SnapRyuNpuConfig& config)
        : runtime{config.runtime_directory}, motion{config.motion_model_path},
          anvil{config.anvil_model_path}, capacity{config.capacity} {
        stats.struct_size = sizeof(stats);
        stats.epoch = 1;
        stats.capacity = capacity;
        stats.state = SNAPRYU_NPU_INITIALIZING;
        thread = std::thread{[this] { Loop(); }};
    }

    ~SnapRyuNpu() {
        {
            std::scoped_lock lock{mutex};
            stopping = true;
            stats.state = SNAPRYU_NPU_STOPPING;
            ++stats.epoch;
            pending.clear();
            completed.clear();
        }
        wake.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }

    void Fail(const char* message) noexcept {
        ++stats.errors;
        stats.state = SNAPRYU_NPU_FAILED;
        std::strncpy(stats.last_error, message, sizeof(stats.last_error) - 1);
        stats.last_error[sizeof(stats.last_error) - 1] = '\0';
        stats.expired_or_reset += pending.size() + completed.size();
        pending.clear();
        completed.clear();
    }

    size_t Occupied() const noexcept { return pending.size() + completed.size() + size_t{active}; }

    // Called with mutex held. Reset clears only queue ownership; an active
    // slot remains reserved until Run finishes, including across epoch changes.
    size_t FreeSlot() const {
        for (size_t slot = 0; slot < capacity; ++slot) {
            if ((!active || active_slot != slot) &&
                std::find(pending.begin(), pending.end(), slot) == pending.end() &&
                std::find(completed.begin(), completed.end(), slot) == completed.end()) {
                return slot;
            }
        }
        throw std::logic_error("NPU slot accounting mismatch");
    }

    void Loop() noexcept {
        try {
            Engine engine{runtime, motion, anvil};
            // The standalone comparison discards its first iteration. Warm the
            // same selected path before READY so its first compilation/run cost
            // cannot poison the per-frame admission policy indefinitely.
            double warmup_job_ms = 0.0;
            {
                const Job warmup{
                    0, 0, 0, 0, std::vector<uint8_t>(RgbBytes), std::vector<uint8_t>(RgbBytes)};
                Completion output;
                engine.Run(warmup, output);
                engine.Run(warmup, output);
                warmup_job_ms = output.info.job_ms;
            }
            {
                std::scoped_lock lock{mutex};
                if (stopping) {
                    return;
                }
                stats.state = SNAPRYU_NPU_READY;
                // Actual second full warmup observation, not a synthetic cost.
                stats.last_job_ms = warmup_job_ms;
            }
            for (;;) {
                size_t slot;
                {
                    std::unique_lock lock{mutex};
                    wake.wait(lock, [this] { return stopping || !pending.empty(); });
                    if (stopping) {
                        return;
                    }
                    slot = pending.front();
                    pending.pop_front();
                    const auto& job = slots[slot].input;
                    if (job.epoch != stats.epoch || NowNs() > job.deadline) {
#if SNAPRYU_DIAGNOSTICS
                        SnapRyuQuality::Capture::Get().Event(job.epoch,job.previous,job.current,
                            "worker_pre_run_drop",job.epoch!=stats.epoch?"epoch_reset":"deadline_expired",NowNs());
#endif
                        ++stats.expired_or_reset;
                        continue;
                    }
                    active = true;
                    active_slot = slot;
                }
                const auto& job = slots[slot].input;
                auto& output = slots[slot].output;
                engine.Run(job, output);
                {
                    std::scoped_lock lock{mutex};
                    active = false;
                    stats.last_job_ms = output.info.job_ms;
                    ++stats.completed;
                    if (stopping) {
                        return;
                    }
                    if (job.epoch != stats.epoch || NowNs() > job.deadline) {
#if SNAPRYU_DIAGNOSTICS
                        SnapRyuQuality::Capture::Get().Event(job.epoch,job.previous,job.current,
                            "worker_post_run_drop",job.epoch!=stats.epoch?"epoch_reset":"deadline_expired",NowNs());
#endif
                        ++stats.expired_or_reset;
                        continue;
                    }
                    completed.push_back(slot);
                }
            }
        } catch (const std::exception& exception) {
            std::scoped_lock lock{mutex};
            active = false;
            if (!stopping) {
                Fail(exception.what());
            }
        } catch (...) {
            std::scoped_lock lock{mutex};
            active = false;
            if (!stopping) {
                Fail("Unknown NPU worker failure");
            }
        }
    }

    fs::path runtime, motion, anvil;
    const uint32_t capacity;
    std::mutex mutex;
    std::condition_variable wake;
    std::array<Slot, 2> slots;
    std::deque<size_t> pending;
    std::deque<size_t> completed;
    SnapRyuNpuStats stats{};
    uint64_t last_accepted_current{};
    bool active{}, stopping{};
    size_t active_slot{};
    std::thread thread;
};

extern "C" {
uint32_t SNAPRYU_NPU_CALL SnapRyuNpuAbiVersion(void) { return SNAPRYU_NPU_ABI; }
int64_t SNAPRYU_NPU_CALL SnapRyuNpuNowNs(void) { return NowNs(); }

int32_t SNAPRYU_NPU_CALL SnapRyuNpuCreate(const SnapRyuNpuConfig* config, SnapRyuNpu** output) {
    if (!output) {
        return SNAPRYU_NPU_INVALID;
    }
    *output = nullptr;
    if (!config || config->struct_size != sizeof(*config) ||
        config->abi_version != SNAPRYU_NPU_ABI || !config->runtime_directory ||
        !config->motion_model_path || !config->anvil_model_path || config->capacity < 1 ||
        config->capacity > 2) {
        return SNAPRYU_NPU_INVALID;
    }
    try {
        if (!fs::path{config->runtime_directory}.is_absolute() ||
            !fs::path{config->motion_model_path}.is_absolute() ||
            !fs::path{config->anvil_model_path}.is_absolute()) {
            return SNAPRYU_NPU_INVALID;
        }
        *output = new SnapRyuNpu{*config};
        return SNAPRYU_NPU_OK;
    } catch (...) {
        return SNAPRYU_NPU_ERROR;
    }
}

void SNAPRYU_NPU_CALL SnapRyuNpuDestroy(SnapRyuNpu* context) { delete context; }

uint64_t SNAPRYU_NPU_CALL SnapRyuNpuReset(SnapRyuNpu* context) {
    if (!context) {
        return 0;
    }
    std::scoped_lock lock{context->mutex};
    ++context->stats.epoch;
    context->stats.expired_or_reset += context->pending.size() + context->completed.size();
    context->pending.clear();
    context->completed.clear();
    context->last_accepted_current = 0;
    return context->stats.epoch;
}

int32_t SNAPRYU_NPU_CALL SnapRyuNpuSubmit(SnapRyuNpu* context, const SnapRyuNpuPair* pair) {
    if (!context || !pair || pair->struct_size != sizeof(*pair) || !pair->previous_id ||
        pair->previous_id == UINT64_MAX || pair->current_id != pair->previous_id + 1 ||
        !ValidImage(pair->previous) || !ValidImage(pair->current)) {
        return SNAPRYU_NPU_INVALID;
    }
    try {
        std::unique_lock lock{context->mutex, std::try_to_lock};
        if (!lock) {
            return SNAPRYU_NPU_BUSY;
        }
        if (context->stopping || context->stats.state == SNAPRYU_NPU_FAILED) {
            return SNAPRYU_NPU_UNAVAILABLE;
        }
        if (context->stats.state != SNAPRYU_NPU_READY) {
            return SNAPRYU_NPU_WARMING;
        }
        if (pair->epoch != context->stats.epoch || NowNs() > pair->deadline_ns ||
            pair->current_id <= context->last_accepted_current) {
            return SNAPRYU_NPU_STALE;
        }
        if (context->Occupied() >= context->capacity) {
            ++context->stats.busy;
            return SNAPRYU_NPU_BUSY;
        }
        const auto slot = context->FreeSlot();
        auto& job = context->slots[slot].input;
        job.epoch = pair->epoch;
        job.previous = pair->previous_id;
        job.current = pair->current_id;
        job.deadline = pair->deadline_ns;
        CopyRgb(pair->previous, job.a);
        CopyRgb(pair->current, job.b);
        if (NowNs() > pair->deadline_ns) {
            ++context->stats.expired_or_reset;
            return SNAPRYU_NPU_STALE;
        }
        context->pending.push_back(slot);
        context->last_accepted_current = pair->current_id;
        ++context->stats.accepted;
        lock.unlock();
        context->wake.notify_one();
        return SNAPRYU_NPU_OK;
    } catch (...) {
        return SNAPRYU_NPU_ERROR;
    }
}

int32_t SNAPRYU_NPU_CALL SnapRyuNpuTake(SnapRyuNpu* context, SnapRyuNpuOutput* output,
                                        uint8_t* rgba, uint64_t data_bytes, uint32_t row_bytes) {
    if (!context || !output || output->struct_size != sizeof(*output) || !rgba ||
        row_bytes < SNAPRYU_NPU_WIDTH * 4 ||
        data_bytes < uint64_t{row_bytes} * (SNAPRYU_NPU_HEIGHT - 1) + SNAPRYU_NPU_WIDTH * 4) {
        return SNAPRYU_NPU_INVALID;
    }
    try {
        std::unique_lock lock{context->mutex, std::try_to_lock};
        if (!lock) {
            return SNAPRYU_NPU_BUSY;
        }
        while (!context->completed.empty()) {
            const auto& front = context->slots[context->completed.front()].output;
            if (front.info.epoch != context->stats.epoch || NowNs() > front.info.deadline_ns) {
                context->completed.pop_front();
                ++context->stats.expired_or_reset;
                continue;
            }
            *output = front.info;
            for (size_t y = 0; y < SNAPRYU_NPU_HEIGHT; ++y) {
                std::memcpy(rgba + y * row_bytes, front.rgba.data() + y * SNAPRYU_NPU_WIDTH * 4,
                            SNAPRYU_NPU_WIDTH * 4);
            }
            context->completed.pop_front();
            ++context->stats.delivered;
            return SNAPRYU_NPU_OK;
        }
        return context->stats.state == SNAPRYU_NPU_FAILED ? SNAPRYU_NPU_UNAVAILABLE
                                                          : SNAPRYU_NPU_EMPTY;
    } catch (...) {
        return SNAPRYU_NPU_ERROR;
    }
}

int32_t SNAPRYU_NPU_CALL SnapRyuNpuGetStats(SnapRyuNpu* context, SnapRyuNpuStats* output) {
    if (!context || !output || output->struct_size != sizeof(*output)) {
        return SNAPRYU_NPU_INVALID;
    }
    std::unique_lock lock{context->mutex, std::try_to_lock};
    if (!lock) {
        return SNAPRYU_NPU_BUSY;
    }
    *output = context->stats;
    output->occupied = static_cast<uint32_t>(context->Occupied());
    return SNAPRYU_NPU_OK;
}
} // extern "C"
