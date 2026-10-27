#include "HardwareEncoder.h"
#include "VideoFrames.h"
#include "VideoTrace.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace CemuPad::Media {
namespace {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
void Check(HRESULT result){if(FAILED(result)){if(VideoTraceEnabled())VideoTrace().encoderHresult=static_cast<std::uint32_t>(result);throw std::runtime_error("Hardware encoder HRESULT "+std::to_string(static_cast<unsigned long>(result)));}}
struct Foundation {
    bool com=false,mf=false;
    Foundation(){Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED));com=true;
        auto result=MFStartup(MF_VERSION,MFSTARTUP_LITE);if(FAILED(result)){CoUninitialize();com=false;Check(result);}mf=true;}
    ~Foundation(){if(mf)MFShutdown();if(com)CoUninitialize();}
};
using SubmittedFrames=std::map<LONGLONG,std::pair<std::uint64_t,Clock::time_point>>;
std::optional<EncodedFrame> ReadEncodedSample(IMFSample* output,SubmittedFrames& submitted,
    Bytes& cachedSps,Bytes& cachedPps,std::uint64_t& dropped){
    DWORD size=0;Check(output->GetTotalLength(&size));
    if(VideoTraceEnabled()){VideoTrace().encodedBytes=size;VideoTrace().encodedNals=0;}
    if(size==0){
        // A hardware MFT can complete a sample without coded picture data.
        // This input was skipped before producing a coded picture, so no
        // decoder reference was lost. Retire it without forcing an IDR:
        // repeated IDRs after rate-control skips can cause further skips.
        LONGLONG pts=0;
        if(SUCCEEDED(output->GetSampleTime(&pts))){dropped+=submitted.erase(pts);if(VideoTraceEnabled())VideoTrace().outputPts=pts;}
        if(VideoTraceEnabled())++VideoTrace().encoderZeroOutputs;
        return {};
    }
    if(size>2*1024*1024)throw std::runtime_error("Encoder output too large");
    ComPtr<IMFMediaBuffer> buffer;Check(output->ConvertToContiguousBuffer(&buffer));BYTE* bytes=nullptr;Check(buffer->Lock(&bytes,nullptr,&size));
    Bytes encoded;try{if(size<=2*1024*1024)encoded.assign(bytes,bytes+size);}catch(...){buffer->Unlock();throw;}Check(buffer->Unlock());
    auto nals=SplitH264(encoded);bool slice=false;
    if(VideoTraceEnabled())VideoTrace().encodedNals=static_cast<std::uint32_t>(nals.size());
    for(const auto& nal:nals){slice|=(nal[0]&31)==1||(nal[0]&31)==5;if((nal[0]&31)==7)cachedSps=nal;if((nal[0]&31)==8)cachedPps=nal;}
    if(VideoTraceEnabled()){VideoTrace().spsBytes=static_cast<std::uint32_t>(cachedSps.size());VideoTrace().ppsBytes=static_cast<std::uint32_t>(cachedPps.size());}
    if(!slice)return {};
    LONGLONG pts=0;Check(output->GetSampleTime(&pts));
    if(VideoTraceEnabled()){VideoTrace().outputPts=pts;VideoTrace().oldestPts=submitted.empty()?-1:submitted.begin()->first;VideoTrace().newestPts=submitted.empty()?-1:submitted.rbegin()->first;}
    auto found=submitted.find(pts);if(found==submitted.end())throw std::runtime_error("Unmatched encoder PTS");
    // Input PTS is strictly increasing and Configure verifies B frames = 0.
    // Once a later picture arrives, older inputs were skipped by this encoder.
    // This also retires a skipped sample whose empty output had no timestamp.
    while(submitted.begin()!=found){submitted.erase(submitted.begin());++dropped;}
    EncodedFrame frame{std::move(nals),found->second.first,cachedSps,cachedPps};submitted.erase(found);return frame;
}
struct EncoderOwner {
    ComPtr<IMFActivate> activation;ComPtr<IMFTransform> transform;ComPtr<IMFMediaEventGenerator> events;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11VideoDevice> video;
    ComPtr<ID3D11VideoContext> videoContext;ComPtr<IMFDXGIDeviceManager> manager;ComPtr<ICodecAPI> codec;
    ComPtr<ID3D11VideoProcessorEnumerator> enumeration;ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> source;ComPtr<ID3D11VideoProcessorInputView> inputView;
    unsigned sourceWidth=0,sourceHeight=0;VideoQuality quality;Bytes bgra,cachedSps,cachedPps;
    bool directRgba=false;
    PixelOrder capturedOrder=PixelOrder::Rgba;
    const bool allowRgba=[] {auto p=std::getenv("CEMUPAD_RGBA_UPLOAD");return !p||p[0]!='0';}();
    MFT_OUTPUT_STREAM_INFO outputInfo{};
    explicit EncoderOwner(VideoQuality q):quality(q){
        MFT_REGISTER_TYPE_INFO in{MFMediaType_Video,MFVideoFormat_NV12},out{MFMediaType_Video,MFVideoFormat_H264};
        IMFActivate** list=nullptr;UINT32 count=0;Check(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,MFT_ENUM_FLAG_HARDWARE|MFT_ENUM_FLAG_SORTANDFILTER,&in,&out,&list,&count));
        if(!count){CoTaskMemFree(list);throw std::runtime_error("No hardware encoder");}
        activation.Attach(list[0]);for(UINT32 i=1;i<count;++i)list[i]->Release();CoTaskMemFree(list);
        Check(activation->ActivateObject(IID_PPV_ARGS(&transform)));
        try{Configure();}catch(...){transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH,0);activation->ShutdownObject();throw;}
    }
    ~EncoderOwner(){if(transform){transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH,0);transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,0);}if(activation)activation->ShutdownObject();}
    void Configure(){
        ComPtr<IMFAttributes> attributes;Check(transform->GetAttributes(&attributes));UINT32 async=0;Check(attributes->GetUINT32(MF_TRANSFORM_ASYNC,&async));
        if(!async)throw std::runtime_error("Hardware encoder is not asynchronous");Check(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK,TRUE));
        D3D_FEATURE_LEVEL level;Check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_VIDEO_SUPPORT|D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,&device,&level,&context));
        ComPtr<ID3D10Multithread> multithread;Check(context.As(&multithread));multithread->SetMultithreadProtected(TRUE);
        Check(device.As(&video));Check(context.As(&videoContext));UINT token=0;Check(MFCreateDXGIDeviceManager(&token,&manager));Check(manager->ResetDevice(device.Get(),token));
        Check(transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,reinterpret_cast<ULONG_PTR>(manager.Get())));
        Check(transform.As(&codec));VARIANT value;VariantInit(&value);value.vt=VT_BOOL;value.boolVal=VARIANT_TRUE;Check(codec->SetValue(&CODECAPI_AVLowLatencyMode,&value));
        value.vt=VT_UI4;value.ulVal=0;Check(codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount,&value));
        value.ulVal=quality.fps;codec->SetValue(&CODECAPI_AVEncMPVGOPSize,&value);
        ComPtr<IMFMediaType> output,input;Check(MFCreateMediaType(&output));Check(MFCreateMediaType(&input));
        for(auto* type:{output.Get(),input.Get()}){
            Check(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video));Check(type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive));
            Check(MFSetAttributeSize(type,MF_MT_FRAME_SIZE,quality.width,quality.height));Check(MFSetAttributeRatio(type,MF_MT_FRAME_RATE,quality.fps,1));
            Check(MFSetAttributeRatio(type,MF_MT_PIXEL_ASPECT_RATIO,1,1));Check(type->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT709));
            Check(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235));
        }
        Check(output->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_H264));
        unsigned bitrate=quality.height>=1080?12'000'000:quality.height>=720?6'000'000:3'000'000;
        Check(output->SetUINT32(MF_MT_AVG_BITRATE,bitrate));Check(output->SetUINT32(MF_MT_MPEG2_PROFILE,eAVEncH264VProfile_Base));
        Check(transform->SetOutputType(0,output.Get(),0));Check(input->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_NV12));
        Check(input->SetUINT32(MF_MT_DEFAULT_STRIDE,quality.width));Check(transform->SetInputType(0,input.Get(),0));
        VARIANT actual;VariantInit(&actual);Check(codec->GetValue(&CODECAPI_AVLowLatencyMode,&actual));
        bool low=actual.vt==VT_BOOL&&actual.boolVal==VARIANT_TRUE;VariantClear(&actual);if(!low)throw std::runtime_error("Low latency was not applied");
        VariantInit(&actual);Check(codec->GetValue(&CODECAPI_AVEncMPVDefaultBPictureCount,&actual));
        bool noB=actual.vt==VT_UI4&&actual.ulVal==0;VariantClear(&actual);if(!noB)throw std::runtime_error("B frames were not disabled");
        Check(transform->GetOutputStreamInfo(0,&outputInfo));Check(transform.As(&events));
        Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0));Check(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0));
    }
    bool TrySource(unsigned width,unsigned height,DXGI_FORMAT format){
        UINT flags=0;
        if(FAILED(enumeration->CheckVideoProcessorFormat(format,&flags))||!(flags&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))return false;
        D3D11_TEXTURE2D_DESC texture{};texture.Width=width;texture.Height=height;texture.MipLevels=1;texture.ArraySize=1;texture.Format=format;
        texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> candidate;ComPtr<ID3D11VideoProcessorInputView> candidateView;
        if(FAILED(device->CreateTexture2D(&texture,nullptr,&candidate)))return false;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};view.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;
        if(FAILED(video->CreateVideoProcessorInputView(candidate.Get(),enumeration.Get(),&view,&candidateView)))return false;
        source=std::move(candidate);inputView=std::move(candidateView);directRgba=format==DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    }
    void Upload(const RgbaFrame& frame){
        const auto* pixels=UploadPixels(frame.rgba,frame.order,
            directRgba?PixelOrder::Rgba:PixelOrder::Bgra,bgra);
        context->UpdateSubresource(source.Get(),0,nullptr,pixels,frame.width*4,0);
    }
    ComPtr<IMFSample> Input(const RgbaFrame& frame,LONGLONG pts,bool forceIdr){
        if(sourceWidth!=frame.width||sourceHeight!=frame.height||capturedOrder!=frame.order){
            inputView.Reset();source.Reset();processor.Reset();enumeration.Reset();
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC description{};description.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            description.InputWidth=frame.width;description.InputHeight=frame.height;description.InputFrameRate={quality.fps,1};
            description.OutputWidth=quality.width;description.OutputHeight=quality.height;description.OutputFrameRate={quality.fps,1};description.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            Check(video->CreateVideoProcessorEnumerator(&description,&enumeration));Check(video->CreateVideoProcessor(enumeration.Get(),0,&processor));
            UINT flags=0;Check(enumeration->CheckVideoProcessorFormat(DXGI_FORMAT_NV12,&flags));if(!(flags&D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))throw std::runtime_error("NV12 conversion unavailable");
            const auto native=frame.order==PixelOrder::Bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
            const bool nativeAllowed=frame.order==PixelOrder::Bgra||allowRgba;
            const auto alternate=frame.order==PixelOrder::Bgra?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM;
            const bool alternateAllowed=frame.order==PixelOrder::Rgba||allowRgba;
            if(!(nativeAllowed&&TrySource(frame.width,frame.height,native))
                &&!(alternateAllowed&&TrySource(frame.width,frame.height,alternate)))throw std::runtime_error("RGB conversion unavailable");
            sourceWidth=frame.width;sourceHeight=frame.height;capturedOrder=frame.order;
        }
        Upload(frame);
        D3D11_TEXTURE2D_DESC description{};description.Width=quality.width;description.Height=quality.height;description.MipLevels=1;description.ArraySize=1;
        description.Format=DXGI_FORMAT_NV12;description.SampleDesc.Count=1;description.Usage=D3D11_USAGE_DEFAULT;description.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;Check(device->CreateTexture2D(&description,nullptr,&target));
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};view.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> output;Check(video->CreateVideoProcessorOutputView(target.Get(),enumeration.Get(),&view,&output));
        RECT sourceRect{0,0,static_cast<LONG>(frame.width),static_cast<LONG>(frame.height)},targetRect{0,0,static_cast<LONG>(quality.width),static_cast<LONG>(quality.height)};
        videoContext->VideoProcessorSetStreamFrameFormat(processor.Get(),0,D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        videoContext->VideoProcessorSetStreamAutoProcessingMode(processor.Get(),0,FALSE);
        // Frames are opaque video, including when the captured alpha channel is zero.
        videoContext->VideoProcessorSetStreamAlpha(processor.Get(),0,FALSE,1.0f);
        videoContext->VideoProcessorSetStreamSourceRect(processor.Get(),0,TRUE,&sourceRect);videoContext->VideoProcessorSetStreamDestRect(processor.Get(),0,TRUE,&targetRect);
        videoContext->VideoProcessorSetOutputTargetRect(processor.Get(),TRUE,&targetRect);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};rgb.YCbCr_Matrix=1;rgb.Nominal_Range=2;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};yuv.YCbCr_Matrix=1;yuv.Nominal_Range=1;
        videoContext->VideoProcessorSetStreamColorSpace(processor.Get(),0,&rgb);videoContext->VideoProcessorSetOutputColorSpace(processor.Get(),&yuv);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=inputView.Get();
        {
            auto converted=videoContext->VideoProcessorBlt(processor.Get(),output.Get(),0,1,&stream);
            if(FAILED(converted)&&(directRgba||allowRgba)){
                // Some drivers advertise a format but reject a particular conversion.
                // Try the other RGB order once and retain it until capture changes.
                const auto fallback=directRgba?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
                if(!TrySource(frame.width,frame.height,fallback))Check(converted);
                Upload(frame);stream.pInputSurface=inputView.Get();
                converted=videoContext->VideoProcessorBlt(processor.Get(),output.Get(),0,1,&stream);
            }
            Check(converted);
        }
        context->Flush();
        if(forceIdr){VARIANT value;VariantInit(&value);value.vt=VT_UI4;value.ulVal=1;Check(codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame,&value));}
        ComPtr<IMFMediaBuffer> buffer;Check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),target.Get(),0,FALSE,&buffer));
        ComPtr<IMFSample> sample;Check(MFCreateSample(&sample));Check(sample->AddBuffer(buffer.Get()));Check(sample->SetSampleTime(pts));Check(sample->SetSampleDuration(10'000'000/quality.fps));return sample;
    }
    std::optional<EncodedFrame> Output(SubmittedFrames& submitted,std::uint64_t& dropped){
        ComPtr<IMFSample> supplied;
        if(!(outputInfo.dwFlags&(MFT_OUTPUT_STREAM_PROVIDES_SAMPLES|MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))){
            Check(MFCreateSample(&supplied));ComPtr<IMFMediaBuffer> buffer;
            Check(MFCreateAlignedMemoryBuffer(std::max<DWORD>(outputInfo.cbSize,2*1024*1024),outputInfo.cbAlignment?outputInfo.cbAlignment-1:0,&buffer));Check(supplied->AddBuffer(buffer.Get()));
        }
        MFT_OUTPUT_DATA_BUFFER data{};data.pSample=supplied.Get();DWORD status=0;auto result=transform->ProcessOutput(0,1,&data,&status);
        if(data.pEvents)data.pEvents->Release();
        ComPtr<IMFSample> output;
        if(data.pSample==supplied.Get())output=supplied;else output.Attach(data.pSample);
        if(result==MF_E_TRANSFORM_STREAM_CHANGE){ComPtr<IMFMediaType> type;Check(transform->GetOutputAvailableType(0,0,&type));Check(transform->SetOutputType(0,type.Get(),0));Check(transform->GetOutputStreamInfo(0,&outputInfo));return {};}
        if(result==MF_E_TRANSFORM_NEED_MORE_INPUT)return {};
        Check(result);if(data.dwStatus==MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE||!output)return {};
        return ReadEncodedSample(output.Get(),submitted,cachedSps,cachedPps,dropped);
    }
};
}
struct HardwareEncoder::Impl {
    VideoQuality quality;std::uint64_t generation=0;std::atomic<bool> stop{false},idr{true};std::thread worker;
    mutable std::mutex mutex;std::deque<EncodedFrame> ready;EncoderStatus state;
    explicit Impl(VideoQuality q):quality(q){
        if(!((q.width==854&&q.height==480)||(q.width==1280&&q.height==720)||(q.width==1920&&q.height==1080))||(q.fps!=30&&q.fps!=60))throw std::invalid_argument("Video quality");
        if(VideoTraceEnabled())++VideoTrace().encoderStarts;
        generation=CemuVideoFrames().Start(q.fps);try{worker=std::thread([this]{Run();});}catch(...){CemuVideoFrames().Stop();throw;}
    }
    ~Impl(){stop=true;if(worker.joinable())worker.join();CemuVideoFrames().Stop();}
    void Run() noexcept{
        try{
            if(VideoTraceEnabled())VideoTrace().encoderStage=1;
            Foundation foundation;if(VideoTraceEnabled())VideoTrace().encoderStage=2;
            EncoderOwner encoder(quality);{std::lock_guard lock(mutex);state.running=true;}if(VideoTraceEnabled())VideoTrace().encoderStage=3;
            SubmittedFrames submitted;unsigned credits=0;std::uint64_t origin=0,lastTimestamp=0;
            while(!stop){
                for(unsigned i=0;i<32;++i){
                    ComPtr<IMFMediaEvent> event;auto result=encoder.events->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event);if(result==MF_E_NO_EVENTS_AVAILABLE)break;Check(result);
                    MediaEventType type;Check(event->GetType(&type));HRESULT status;Check(event->GetStatus(&status));Check(status);
                    if(type==METransformNeedInput)credits=std::min(credits+1,8u);
                    else if(type==METransformHaveOutput){std::uint64_t dropped=0;auto frame=encoder.Output(submitted,dropped);
                        if(dropped){std::lock_guard lock(mutex);state.dropped+=dropped;if(VideoTraceEnabled())VideoTrace().encoderSkippedInputs+=dropped;}
                        if(frame){if(VideoTraceEnabled())++VideoTrace().encoderOutputs;std::lock_guard lock(mutex);++state.outputs;
                        if(ready.size()>=3){ready.pop_front();++state.dropped;}ready.push_back(std::move(*frame));}}
                }
                if(credits&&submitted.size()<3){
                    auto frame=CemuVideoFrames().Take(generation);
                    // Completed GPU slots may publish an older capture after a
                    // newer one. Never submit duplicate or backward timestamps.
                    if(frame&&state.inputs&&frame->timestampUs<=lastTimestamp){std::lock_guard lock(mutex);++state.dropped;frame.reset();}
                    if(frame){if(!origin)origin=frame->timestampUs;lastTimestamp=frame->timestampUs;LONGLONG pts=static_cast<LONGLONG>((frame->timestampUs-origin)*10);
                        auto sample=encoder.Input(*frame,pts,idr.exchange(false));Check(encoder.transform->ProcessInput(0,sample.Get(),0));
                        submitted.emplace(pts,std::pair(frame->timestampUs,Clock::now()));--credits;std::lock_guard lock(mutex);++state.inputs;}
                    if(VideoTraceEnabled()){if(frame)++VideoTrace().encoderInputs;else ++VideoTrace().encoderEmpty;}
                }
                if(VideoTraceEnabled()){VideoTrace().encoderCredits=credits;VideoTrace().submitted=static_cast<std::uint32_t>(submitted.size());}
                if(!submitted.empty()&&Clock::now()-submitted.begin()->second.second>std::chrono::seconds(2))throw std::runtime_error("Encoder output stalled");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }catch(const std::exception& error){if(VideoTraceEnabled()){
            const std::string_view message(error.what());
            VideoTrace().encoderFailure=message.starts_with("Hardware encoder HRESULT")?1:message=="Invalid H264 data"?2:message=="Unmatched encoder PTS"?3:message=="Encoder output stalled"?4:5;
            ++VideoTrace().encoderFailed;VideoTrace().encoderStage=9;
        }std::lock_guard lock(mutex);state.failure=error.what();}
        catch(...){if(VideoTraceEnabled()){++VideoTrace().encoderFailed;VideoTrace().encoderStage=9;}std::lock_guard lock(mutex);state.failure="Hardware encoder failure";}
        {std::lock_guard lock(mutex);state.running=false;}
    }
};
HardwareEncoder::HardwareEncoder(VideoQuality q):m(std::make_unique<Impl>(q)){}
HardwareEncoder::~HardwareEncoder()=default;
std::optional<EncodedFrame> HardwareEncoder::Poll(){std::lock_guard lock(m->mutex);if(m->ready.empty())return {};auto frame=std::move(m->ready.front());m->ready.pop_front();return frame;}
EncoderStatus HardwareEncoder::Status() const{std::lock_guard lock(m->mutex);return m->state;}
void HardwareEncoder::RequestIdr(){m->idr=true;}
}
