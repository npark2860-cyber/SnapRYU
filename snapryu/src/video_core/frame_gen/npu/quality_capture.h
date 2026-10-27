// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#if SNAPRYU_DIAGNOSTICS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace SnapRyuQuality {
// Private diagnostic transport; never changes ABI 1 or accesses borrowed tensors.
class Capture {
    using Key = std::tuple<uint64_t, uint64_t, uint64_t>;
    struct Pair {
        size_t raw_limit{}, raw_bytes{};
        std::set<std::string> stages;
    };
    struct Blob {
        Key key;
        std::string name, description;
        std::vector<uint8_t> data;
        uint32_t width{}, height{}, stride{};
        int64_t time_ns{};
    };
public:
    static Capture& Get() { static Capture capture; return capture; }
    bool Enabled() const { return enabled; }
    void Event(uint64_t e,uint64_t a,uint64_t b,const char* stage,const char* reason,
               int64_t observed_ns=0) noexcept {
        if (!enabled) return;
        try {
            Add(e,a,b,stage,nullptr,0,0,0,0,"{\"reason\":\""+std::string(reason)+
                "\",\"observed_ns\":"+std::to_string(observed_ns)+"}");
        } catch (...) {}
    }
    void Tag(void* frame,uint64_t e,uint64_t a,uint64_t b) noexcept {
        if (!enabled) return;
        try { std::scoped_lock lock{mutex}; tags[frame]={e,a,b}; } catch (...) {}
    }
    void Presented(void* frame,bool accepted,uint32_t w,uint32_t h,
                   const char* phase="swapchain_present") noexcept {
        if (!enabled) return;
        try {
        Key key;
        {
            std::scoped_lock lock{mutex};
            auto found=tags.find(frame);
            if (found==tags.end()) return;
            key=found->second;tags.erase(found);
        }
        const auto [e,a,b]=key;
        Add(e,a,b,"present_result",nullptr,0,0,0,0,
            "{\"accepted\":"+std::string(accepted?"true":"false")+
            ",\"phase\":\""+std::string(phase)+"\""+
            ",\"swapchain_width\":"+std::to_string(w)+",\"swapchain_height\":"+std::to_string(h)+
            ",\"scanout_verified\":false}");
        } catch (...) {}
    }
    void Source(std::string metadata) noexcept {
        if (!enabled) return;
        try { std::scoped_lock lock{mutex}; source_metadata=std::move(metadata); } catch (...) {}
    }
    bool Selected(uint64_t e, uint64_t a, uint64_t b) {
        std::scoped_lock lock{mutex};
        return pairs.contains({e,a,b});
    }
    bool Select(uint64_t e, uint64_t a, uint64_t b, uint32_t w, uint32_t h,
                const std::string& metadata,size_t requested_raw=0,size_t requested_gpu=0) noexcept {
        if (!enabled || !armed.load() || b < start_id || b < next_id || a + 1 != b) return false;
        try {
            std::scoped_lock lock{mutex};
            const Key key{e,a,b};
            if (pairs.contains(key)) return false;
            if (pairs.size() >= max_pairs) {
                Push(key,"capture_limit",nullptr,0,0,0,0,
                     "{\"reason\":\"maximum 4 capture pairs\"}");
                next_id=UINT64_MAX;
                return false;
            }
            // CPU tensors + masks/debug; two original/full generated + uploaded 720p.
            const size_t gpu = requested_raw?requested_gpu:size_t{w} * h * 12 + size_t{1280} * 720 * 4;
            const size_t cpu = size_t{1280} * 720 * 20 + 131072;
            const size_t raw = requested_raw?requested_raw:gpu + cpu;
            if(raw>Budget||gpu>Budget-raw)return false;
            const size_t memory = gpu + raw;
            if (w == 0 || h == 0 || w > 8192 || h > 8192 || memory > Budget - reserved) {
                Push(key,"capture_limit",nullptr,0,0,0,0,
                     "{\"reason\":\"256MiB owned-memory reservation limit\"}");
                next_id = UINT64_MAX;
                return false;
            }
            pairs.emplace(key, Pair{raw,0,{}});
            reserved += memory;
            next_id = b + spacing;
            Push(key,"pair",nullptr,0,0,0,0,"{\"renderer\":"+metadata+
                 ",\"source\":"+source_metadata+",\"reserved_bytes\":"+std::to_string(memory)+"}");
            return true;
        } catch (...) { return false; }
    }
    void Add(uint64_t e, uint64_t a, uint64_t b, const std::string& name,
             const void* data, size_t bytes, uint32_t w=0, uint32_t h=0,
             uint32_t stride=0, const std::string& description="{}") noexcept {
        if (!enabled || (bytes && !data)) return;
        try {
            std::scoped_lock lock{mutex};
            const Key key{e,a,b};
            auto found = pairs.find(key);
            if (found == pairs.end()) return;
            auto& pair = found->second;
            if (pair.stages.contains(name)) return;
            if (bytes > pair.raw_limit-pair.raw_bytes) { ++budget_skips;return; }
            if (queue.size() >= 64 || pair.stages.size() >= 24) { ++queue_skips;return; }
            Push(key,name,data,bytes,w,h,stride,description);
            pair.stages.insert(name);
            pair.raw_bytes += bytes;
        } catch (...) {}
    }
private:
    static constexpr size_t Budget = size_t{256} * 1024 * 1024;
    static std::wstring Environment(const wchar_t* name) {
        std::array<wchar_t,32768> text{};
        const DWORD n = GetEnvironmentVariableW(name,text.data(),DWORD(text.size()));
        return n && n < text.size() ? std::wstring(text.data(),n) : std::wstring{};
    }
    Capture() noexcept {
        try {
            const auto target = Environment(L"SNAPRYU_QUALITY_CAPTURE_DIR");
            if (target.empty()) return;
            std::array<wchar_t,32768> exe{};
            const auto n = GetModuleFileNameW(nullptr,exe.data(),DWORD(exe.size()));
            if (!n || n >= exe.size()) return;
            auto project = std::filesystem::path{std::wstring(exe.data(),n)}.parent_path();
            while (!project.empty() && !std::filesystem::exists(
                       project/"planning/SOL-NPU-QUALITY-TASKS-20261014.txt")) {
                const auto parent=project.parent_path();
                if (parent==project) return;
                project=parent;
            }
            if (project.empty()) return;
            root=std::filesystem::weakly_canonical(std::filesystem::path{target});
            bool permitted=false;
            for(const auto* directory:{"reports/npu-quality-sol/captures","reports/npu-cemu-parity-sol/captures"}){
                const auto base=std::filesystem::weakly_canonical(project/directory);
                const auto rel=root.lexically_relative(base);
                bool inside=root.is_absolute()&&!rel.empty()&&!rel.is_absolute();
                for(const auto& part:rel)if(part=="..")inside=false;
                permitted|=inside;
            }
            if(!permitted)return;
            // A fresh run folder prevents overwriting an earlier capture.
            if (std::filesystem::exists(root)) return;
            const auto requested=Environment(L"SNAPRYU_QUALITY_START_ID");
            if (!requested.empty()) start_id=std::stoull(requested);
            wait_for_arm=Environment(L"SNAPRYU_QUALITY_WAIT_FOR_ARM")==L"1";
            armed.store(!wait_for_arm);
            std::filesystem::create_directories(root);
            enabled=true;
            writer=std::thread{[this] { WriteLoop(); }};
        } catch (...) { enabled=false; }
    }
    ~Capture() {
        { std::scoped_lock lock{mutex}; stopping=true; }
        wake.notify_all();
        if (writer.joinable()) writer.join();
    }
    void Push(const Key& key,const std::string& name,const void* data,size_t bytes,
              uint32_t w,uint32_t h,uint32_t stride,const std::string& metadata) {
        Blob blob{key,name,metadata,{},w,h,stride,0};
        LARGE_INTEGER now{},frequency{};
        QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
        blob.time_ns=int64_t(double(now.QuadPart)*1e9/double(frequency.QuadPart));
        if (bytes) {
            const auto* first=static_cast<const uint8_t*>(data);
            blob.data.assign(first,first+bytes);
        }
        queue.push_back(std::move(blob));
        wake.notify_one();
    }
    void WriteLoop() noexcept {
        // Only this owned I/O thread polls the manual trigger. Render/NPU threads
        // check an atomic flag; loading frames cannot consume the capture budget.
        auto state=[this] {
            try {
                std::ofstream(root/"capture-state.json")<<"{\"wait_for_arm\":"
                    <<(wait_for_arm?"true":"false")<<",\"armed\":"
                    <<(armed.load()?"true":"false")<<"}\n";
            } catch (...) {}
        };
        state();
        for (;;) {
            if (!armed.load()) {
                std::error_code error;
                if (std::filesystem::is_regular_file(root.parent_path()/"capture-arm.request",error)) {
                    armed.store(true);
                    state();
                }
            }
            Blob blob;
            {
                std::unique_lock lock{mutex};
                wake.wait_for(lock,std::chrono::milliseconds(100),[this]{return stopping||!queue.empty();});
                if (queue.empty() && stopping) {
                    try {
                        std::ofstream(root/"capture-summary.json")<<"{\"pairs\":"<<pairs.size()
                            <<",\"reserved_bytes\":"<<reserved<<",\"budget_bytes\":"<<Budget
                            <<",\"queue_or_stage_skips\":"<<queue_skips
                            <<",\"raw_budget_skips\":"<<budget_skips
                            <<",\"wait_for_arm\":"<<(wait_for_arm?"true":"false")
                            <<",\"armed\":"<<(armed.load()?"true":"false")<<"}\n";
                    } catch (...) {}
                    return;
                }
                if (queue.empty()) continue;
                blob=std::move(queue.front());queue.pop_front();
            }
            try {
                const auto [e,a,b]=blob.key;
                const auto folder=root/("e"+std::to_string(e)+"-a"+std::to_string(a)+"-b"+std::to_string(b));
                std::filesystem::create_directories(folder);
                if (!blob.data.empty()) {
                    std::ofstream file(folder/(blob.name+".bin"),std::ios::binary);
                    file.write(reinterpret_cast<const char*>(blob.data.data()),std::streamsize(blob.data.size()));
                    if (!file) throw std::runtime_error("quality raw write failed");
                }
                std::ofstream meta(folder/(blob.name+".json"));
                meta << "{\"epoch\":"<<e<<",\"previous_id\":"<<a<<",\"current_id\":"<<b
                     <<",\"stage\":\""<<blob.name<<"\",\"width\":"<<blob.width
                     <<",\"height\":"<<blob.height<<",\"stride\":"<<blob.stride
                     <<",\"bytes\":"<<blob.data.size()<<",\"snapshot_qpc_ns\":"<<blob.time_ns
                     <<",\"detail\":"<<blob.description<<"}\n";
                if (!meta) throw std::runtime_error("quality metadata write failed");
            } catch (...) {
                // Diagnostic I/O must never fail the renderer or NPU worker.
                try { std::ofstream(root/"WRITE-ERROR.txt",std::ios::app)<<"capture write failed\n"; } catch (...) {}
            }
        }
    }
    std::filesystem::path root;
    bool enabled{},stopping{};
    bool wait_for_arm{};
    std::atomic<bool> armed{true};
    uint64_t start_id{120},next_id{},spacing{30};
    size_t max_pairs{4},reserved{};
    size_t queue_skips{},budget_skips{};
    std::mutex mutex;
    std::condition_variable wake;
    std::map<Key,Pair> pairs;
    std::map<void*,Key> tags;
    std::deque<Blob> queue;
    std::thread writer;
    std::string source_metadata{"{}"};
};
} // namespace SnapRyuQuality
#endif
