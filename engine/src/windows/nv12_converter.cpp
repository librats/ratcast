#include "windows/nv12_converter.h"

#include <mutex>
#include <utility>
#include <vector>

#include "librats/util/logger.h"

namespace ratcast::d3d11 {
namespace {

using win::ComPtr;

/// A texture pictures are drawn into, and the processor's view of it.
struct Slot {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11VideoProcessorOutputView> view;
};

/// The slots of one size; frames give theirs back from any thread, and slots
/// of a size given up since are let go.
struct Pool {
    std::mutex mutex;
    uint64_t generation = 0;
    size_t out = 0;  ///< slots of this generation held by frames
    std::vector<std::unique_ptr<Slot>> free;

    void give_back(Slot* slot, uint64_t of) {
        std::unique_ptr<Slot> owned(slot);
        std::lock_guard lock(mutex);
        if (of != generation) return;
        --out;
        free.push_back(std::move(owned));
    }
};

D3D11_VIDEO_PROCESSOR_CONTENT_DESC content(uint32_t width, uint32_t height) {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate = {60, 1};
    desc.InputWidth = width;
    desc.InputHeight = height;
    desc.OutputFrameRate = {60, 1};
    desc.OutputWidth = width;
    desc.OutputHeight = height;
    desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    return desc;
}

DXGI_COLOR_SPACE_TYPE dxgi_color_space(ColorSpace color_space) {
    const bool full = color_space.range == ColorRange::Full;
    if (color_space.matrix == ColorMatrix::BT601) {
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                    : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    }
    return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
}

}  // namespace

struct Nv12Converter::State {
    std::shared_ptr<Device> device;
    ComPtr<ID3D11VideoDevice> video_device;
    ComPtr<ID3D11VideoContext> video_context;
    ColorSpace color_space;

    // For the current size.
    uint32_t width = 0;
    uint32_t height = 0;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    ComPtr<ID3D11Texture2D> input;  ///< the picture to convert, copied out of the source's
    ComPtr<ID3D11VideoProcessorInputView> input_view;
    std::shared_ptr<Pool> pool = std::make_shared<Pool>();

    /// Ready for `width` x `height`. Under the device's lock.
    HRESULT resize(uint32_t new_width, uint32_t new_height) {
        width = height = 0;
        input_view.Reset();
        input.Reset();
        processor.Reset();
        enumerator.Reset();
        {
            std::lock_guard lock(pool->mutex);
            ++pool->generation;
            pool->out = 0;
            pool->free.clear();
        }

        const D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = content(new_width, new_height);
        HRESULT result = video_device->CreateVideoProcessorEnumerator(&desc, &enumerator);
        if (FAILED(result)) return result;
        result = video_device->CreateVideoProcessor(enumerator.Get(), 0, &processor);
        if (FAILED(result)) return result;

        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = new_width;
        texture.Height = new_height;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        result = device->get()->CreateTexture2D(&texture, nullptr, &input);
        if (FAILED(result)) return result;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
        view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        result = video_device->CreateVideoProcessorInputView(input.Get(), enumerator.Get(), &view,
                                                             &input_view);
        if (FAILED(result)) return result;

        // Colours as asked, nothing done to the picture beyond that.
        ComPtr<ID3D11VideoContext1> context1;
        if (SUCCEEDED(video_context.As(&context1))) {
            context1->VideoProcessorSetStreamColorSpace1(processor.Get(), 0,
                                                         DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            context1->VideoProcessorSetOutputColorSpace1(processor.Get(),
                                                         dxgi_color_space(color_space));
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
            in.RGB_Range = 0;  // full
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
            out.YCbCr_Matrix = color_space.matrix == ColorMatrix::BT709 ? 1 : 0;
            out.Nominal_Range = color_space.range == ColorRange::Full
                                    ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                    : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            video_context->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &in);
            video_context->VideoProcessorSetOutputColorSpace(processor.Get(), &out);
        }
        video_context->VideoProcessorSetStreamFrameFormat(processor.Get(), 0,
                                                          D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
        width = new_width;
        height = new_height;
        return S_OK;
    }

    /// A slot to draw into, held by what it returns; none when all are out.
    std::shared_ptr<const Slot> take(HRESULT& result) {
        std::unique_ptr<Slot> slot;
        uint64_t generation = 0;
        {
            std::lock_guard lock(pool->mutex);
            if (pool->free.empty() && pool->out == kPictures) return nullptr;
            ++pool->out;
            generation = pool->generation;
            if (!pool->free.empty()) {
                slot = std::move(pool->free.back());
                pool->free.pop_back();
            }
        }
        if (!slot) {
            slot = std::make_unique<Slot>();
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width = width;
            texture.Height = height;
            texture.MipLevels = 1;
            texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_NV12;
            texture.SampleDesc.Count = 1;
            texture.Usage = D3D11_USAGE_DEFAULT;
            texture.BindFlags = D3D11_BIND_RENDER_TARGET;
            result = device->get()->CreateTexture2D(&texture, nullptr, &slot->texture);
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
            view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(result)) {
                result = video_device->CreateVideoProcessorOutputView(
                    slot->texture.Get(), enumerator.Get(), &view, &slot->view);
            }
            if (FAILED(result)) {
                std::lock_guard lock(pool->mutex);
                if (generation == pool->generation) --pool->out;
                return nullptr;
            }
        }
        return std::shared_ptr<const Slot>(
            slot.release(), [pool = pool, generation](const Slot* given) {
                pool->give_back(const_cast<Slot*>(given), generation);
            });
    }
};

Result<std::unique_ptr<Nv12Converter>> Nv12Converter::create(std::shared_ptr<Device> device,
                                                             ColorSpace color_space) {
    if (!device->video()) return fail(Errc::Unavailable);
    auto state = std::make_unique<State>();
    if (FAILED(device->get()->QueryInterface(IID_PPV_ARGS(&state->video_device))) ||
        FAILED(device->context()->QueryInterface(IID_PPV_ARGS(&state->video_context)))) {
        return fail(Errc::Unavailable);
    }
    // Whether this processor takes BGRA in and gives NV12 out at all.
    const D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = content(64, 64);
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    UINT in = 0;
    UINT out = 0;
    if (FAILED(state->video_device->CreateVideoProcessorEnumerator(&desc, &enumerator)) ||
        FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &in)) ||
        FAILED(enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &out)) ||
        !(in & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !(out & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        LOG_DEBUG("engine", "the GPU's video processor cannot turn BGRA into NV12");
        return fail(Errc::Unavailable);
    }
    state->device = std::move(device);
    state->color_space = color_space;
    return std::unique_ptr<Nv12Converter>(new Nv12Converter(std::move(state)));
}

Nv12Converter::Nv12Converter(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

Nv12Converter::~Nv12Converter() = default;

Result<VideoFrame> Nv12Converter::convert(ID3D11Texture2D& bgra, uint32_t width, uint32_t height,
                                          Timestamp captured) {
    State& state = *state_;
    const Lock lock(*state.device);
    if (width != state.width || height != state.height) {
        if (const HRESULT result = state.resize(width, height); FAILED(result)) {
            LOG_WARN("engine", "no video processor for " << width << "x" << height << ": 0x"
                                                         << std::hex << unsigned(result));
            return fail(win::errc_of(result));
        }
    }
    HRESULT result = S_OK;
    std::shared_ptr<const Slot> slot = state.take(result);
    if (FAILED(result)) return fail(win::errc_of(result));
    if (!slot) return VideoFrame{};

    const D3D11_BOX box{0, 0, 0, width, height, 1};
    state.device->context()->CopySubresourceRegion(state.input.Get(), 0, 0, 0, 0, &bgra, 0, &box);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = state.input_view.Get();
    result = state.video_context->VideoProcessorBlt(state.processor.Get(), slot->view.Get(), 0, 1,
                                                    &stream);
    if (FAILED(result)) return fail(win::errc_of(result));
    state.device->context()->Flush();  // under way while the picture goes to its encoder
    ComPtr<ID3D11Texture2D> texture = slot->texture;
    return VideoFrame(
        PixelFormat::NV12, width, height,
        std::make_shared<Picture>(state.device, std::move(texture), 0, std::move(slot)), captured,
        state.color_space);
}

}  // namespace ratcast::d3d11
