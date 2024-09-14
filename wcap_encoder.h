#pragma once

#include "wcap.h"
#include "wcap_config.h"
#include "wcap_tex_resize.h"
#include "wcap_yuv_convert.h"

#include <d3d11.h>
#include <mfidl.h>
#include <mfreadwrite.h>

//
// interface
//

#define ENCODER_VIDEO_BUFFER_COUNT 8
#define ENCODER_AUDIO_BUFFER_COUNT 16

typedef struct
{
	DWORD InputWidth;   // width to what input will be cropped
	DWORD InputHeight;  // height to what input will be cropped
	DWORD OutputWidth;  // width of video output
	DWORD OutputHeight; // height of video output
	DWORD FramerateNum; // video output framerate numerator
	DWORD FramerateDen; // video output framerate denumerator
	UINT64 StartTime;   // time in QPC ticks since first call of NewFrame

	IMFAsyncCallback VideoSampleCallback;
	IMFAsyncCallback AudioSampleCallback;
	ID3D11Device* Device;
	ID3D11DeviceContext* Context;
	IMFSinkWriter* Writer;
	int VideoStreamIndex;
	int AudioStreamIndex;

	ID3D11RenderTargetView* InputView;

	TexResize Resize;
	YuvConvert Convert;

	YuvConvertOutput ConvertOutput[ENCODER_VIDEO_BUFFER_COUNT];
	IMFSample*       VideoSample[ENCODER_VIDEO_BUFFER_COUNT];

	BOOL   VideoDiscontinuity;
	UINT64 VideoLastTime;
	DWORD  VideoIndex; // next index to use
	LONG   VideoCount; // how many samples are currently available to use

	IMFTransform*   Resampler;
	IMFSample*      AudioSample[ENCODER_AUDIO_BUFFER_COUNT];
	IMFSample*      AudioInputSample;
	IMFMediaBuffer* AudioInputBuffer;
	DWORD           AudioFrameSize;
	DWORD           AudioSampleRate;
	DWORD           AudioIndex; // next index to use
	LONG            AudioCount; // how many samples are currently available to use
}
Encoder;

typedef struct
{
	DWORD Width;
	DWORD Height;
	DWORD FramerateNum;
	DWORD FramerateDen;
	WAVEFORMATEX* AudioFormat;
	Config* Config;
}
EncoderConfig;

static void Encoder_Init(Encoder* Encoder);
static BOOL Encoder_Start(Encoder* Encoder, ID3D11Device* Device, LPWSTR FileName, const EncoderConfig* Config);
static void Encoder_Stop(Encoder* Encoder);

static BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod);
static void Encoder_NewSamples(Encoder* Encoder, LPCVOID Samples, DWORD VideoCount, UINT64 Time, UINT64 TimePeriod);
static void Encoder_Update(Encoder* Encoder, UINT64 Time, UINT64 TimePeriod);
static void Encoder_GetStats(Encoder* Encoder, DWORD* Bitrate, DWORD* LengthMsec, UINT64* FileSize);

//
// implementation
//

#include <evr.h>
#include <cguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>

#define MFT64(high, low) (((UINT64)high << 32) | (low))

static HRESULT STDMETHODCALLTYPE Encoder__QueryInterface(IMFAsyncCallback* this, REFIID riid, void** Object)
{
	if (Object == NULL)
	{
		return E_POINTER;
	}
	if (IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_IMFAsyncCallback, riid))
	{
		*Object = this;
		return S_OK;
	}
	return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Encoder__AddRef(IMFAsyncCallback* this)
{
	return 1;
}

static ULONG STDMETHODCALLTYPE Encoder__Release(IMFAsyncCallback* this)
{
	return 1;
}

static HRESULT STDMETHODCALLTYPE Encoder__GetParameters(IMFAsyncCallback* this, DWORD* Flags, DWORD* Queue)
{
	*Flags = 0;
	*Queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Encoder__VideoInvoke(IMFAsyncCallback* this, IMFAsyncResult* Result)
{
	IUnknown* Object;
	IMFSample* Sample;

	HR(IMFAsyncResult_GetObject(Result, &Object));
	HR(IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample));
	IUnknown_Release(Object);
	// keep Sample object reference count incremented to reuse for new frame submission

	Encoder* Enc = CONTAINING_RECORD(this, Encoder, VideoSampleCallback);
	InterlockedIncrement(&Enc->VideoCount);

	return S_OK;
}

static HRESULT STDMETHODCALLTYPE Encoder__AudioInvoke(IMFAsyncCallback* this, IMFAsyncResult* Result)
{
	IUnknown* Object;
	IMFSample* Sample;

	HR(IMFAsyncResult_GetObject(Result, &Object));
	HR(IUnknown_QueryInterface(Object, &IID_IMFSample, (LPVOID*)&Sample));
	IUnknown_Release(Object);
	// keep Sample object reference count incremented to reuse for new sample submission

	Encoder* Enc = CONTAINING_RECORD(this, Encoder, AudioSampleCallback);
	InterlockedIncrement(&Enc->AudioCount);
	WakeByAddressSingle(&Enc->AudioCount);

	return S_OK;
}

static IMFAsyncCallbackVtbl Encoder__VideoSampleCallbackVtbl =
{
	&Encoder__QueryInterface,
	&Encoder__AddRef,
	&Encoder__Release,
	&Encoder__GetParameters,
	&Encoder__VideoInvoke,
};

static IMFAsyncCallbackVtbl Encoder__AudioSampleCallbackVtbl =
{
	&Encoder__QueryInterface,
	&Encoder__AddRef,
	&Encoder__Release,
	&Encoder__GetParameters,
	&Encoder__AudioInvoke,
};

static void Encoder__OutputAudioSamples(Encoder* Encoder)
{
	for (;;)
	{
		// we don't want to drop any audio frames, so wait for available sample/buffer
		LONG Count = Encoder->AudioCount;
		while (Count == 0)
		{
			LONG Zero = 0;
			WaitOnAddress(&Encoder->AudioCount, &Zero, sizeof(LONG), INFINITE);
			Count = Encoder->AudioCount;
		}

		DWORD Index = Encoder->AudioIndex;

		IMFSample* Sample = Encoder->AudioSample[Index];

		DWORD Status;
		MFT_OUTPUT_DATA_BUFFER Output = { .dwStreamID = 0, .pSample = Sample };
		HRESULT hr = IMFTransform_ProcessOutput(Encoder->Resampler, 0, 1, &Output, &Status);
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			// no output is available
			break;
		}
		Assert(SUCCEEDED(hr));

		Encoder->AudioIndex = (Index + 1) % ENCODER_AUDIO_BUFFER_COUNT;
		InterlockedDecrement(&Encoder->AudioCount);

		IMFTrackedSample* Tracked;
		HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
		HR(IMFTrackedSample_SetAllocator(Tracked, &Encoder->AudioSampleCallback, (IUnknown*)Tracked));

		HR(IMFSinkWriter_WriteSample(Encoder->Writer, Encoder->AudioStreamIndex, Sample));

		IMFSample_Release(Sample);
		IMFTrackedSample_Release(Tracked);
	}
}

void Encoder_Init(Encoder* Encoder)
{
	HR(MFStartup(MF_VERSION, MFSTARTUP_LITE));
	Encoder->VideoSampleCallback.lpVtbl = &Encoder__VideoSampleCallbackVtbl;
	Encoder->AudioSampleCallback.lpVtbl = &Encoder__AudioSampleCallbackVtbl;
}

BOOL Encoder_Start(Encoder* Encoder, ID3D11Device* Device, LPWSTR FileName, const EncoderConfig* Config)
{
	UINT Token;
	IMFDXGIDeviceManager* Manager;
	HR(MFCreateDXGIDeviceManager(&Token, &Manager));
	HR(IMFDXGIDeviceManager_ResetDevice(Manager, (IUnknown*)Device, Token));

	ID3D11DeviceContext* Context;
	ID3D11Device_GetImmediateContext(Device, &Context);

	DWORD InputWidth = Config->Width;
	DWORD InputHeight = Config->Height;
	DWORD OutputWidth = Config->Config->VideoMaxWidth;
	DWORD OutputHeight = Config->Config->VideoMaxHeight;

	if (OutputWidth != 0 && OutputHeight == 0)
	{
		// limit max width
		if (OutputWidth < InputWidth)
		{
			OutputHeight = InputHeight * OutputWidth / InputWidth;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}
	else if (OutputWidth == 0 && OutputHeight != 0)
	{
		// limit max height
		if (OutputHeight < InputHeight)
		{
			OutputWidth = InputWidth * OutputHeight / InputHeight;
		}
		else
		{
			OutputWidth = InputWidth;
			OutputHeight = InputHeight;
		}
	}
	else if (OutputWidth != 0 && OutputHeight != 0)
	{
		// limit max width and max height
		if (OutputWidth * InputHeight < OutputHeight * InputWidth)
		{
			if (OutputWidth < InputWidth)
			{
				OutputHeight = InputHeight * OutputWidth / InputWidth;
			}
			else
			{
				OutputWidth = InputWidth;
				OutputHeight = InputHeight;
			}
		}
		else
		{
			if (OutputHeight < InputHeight)
			{
				OutputWidth = InputWidth * OutputHeight / InputHeight;
			}
			else
			{
				OutputWidth = InputWidth;
				OutputHeight = InputHeight;
			}
		}
	}
	else
	{
		// use original size
		OutputWidth = InputWidth;
		OutputHeight = InputHeight;
	}

	// must be multiple of 2, round upwards
	InputWidth = (InputWidth + 1) & ~1;
	InputHeight = (InputHeight + 1) & ~1;
	OutputWidth = (OutputWidth + 1) & ~1;
	OutputHeight = (OutputHeight + 1) & ~1;

	BOOL Result = FALSE;
	IMFSinkWriter* Writer = NULL;
	IMFTransform* Resampler = NULL;
	HRESULT hr;

	Encoder->VideoStreamIndex = -1;
	Encoder->AudioStreamIndex = -1;

	const GUID* Container;
	const GUID* Codec;
	UINT32 Profile;
	const GUID* MediaFormatYUV;
	DXGI_FORMAT ConvertFormat;
	if (Config->Config->VideoCodec == CONFIG_VIDEO_H264)
	{
		MediaFormatYUV = &MFVideoFormat_NV12;
		ConvertFormat = DXGI_FORMAT_NV12;

		Container = Config->Config->FragmentedOutput ? &MFTranscodeContainerType_FMPEG4 : &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_H264;
		Profile = ((UINT32[]) { eAVEncH264VProfile_Base, eAVEncH264VProfile_Main, eAVEncH264VProfile_High })[Config->Config->VideoProfile];

	}
	else if (Config->Config->VideoCodec == CONFIG_VIDEO_H265 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN)
	{
		MediaFormatYUV = &MFVideoFormat_NV12;
		ConvertFormat = DXGI_FORMAT_NV12;

		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_HEVC;
		Profile = eAVEncH265VProfile_Main_420_8;

	}
	else // Config->Config->VideoCodec == CONFIG_VIDEO_H265 && Config->Config->VideoProfile == CONFIG_VIDEO_MAIN_10
	{
		MediaFormatYUV = &MFVideoFormat_P010;
		ConvertFormat = DXGI_FORMAT_P010;

		Container = &MFTranscodeContainerType_MPEG4;
		Codec = &MFVideoFormat_HEVC;
		Profile = eAVEncH265VProfile_Main_420_10;
	}

	// output file
	{
		IMFAttributes* Attributes;
		HR(MFCreateAttributes(&Attributes, 4));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, Config->Config->HardwareEncoder));
		HR(IMFAttributes_SetUnknown(Attributes, &MF_SINK_WRITER_D3D_MANAGER, (IUnknown*)Manager));
		HR(IMFAttributes_SetUINT32(Attributes, &MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
		HR(IMFAttributes_SetGUID(Attributes, &MF_TRANSCODE_CONTAINERTYPE, Container));

		hr = MFCreateSinkWriterFromURL(FileName, NULL, Attributes, &Writer);
		IMFAttributes_Release(Attributes);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot create output mp4 file!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video output type
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));

		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, Codec));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_MPEG2_PROFILE, Profile));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_AVG_BITRATE, Config->Config->VideoBitrate * 1000));

		hr = IMFSinkWriter_AddStream(Writer, Type, &Encoder->VideoStreamIndex);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot configure video encoder!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video input type, NV12 or P010 format
	{
		IMFMediaType* Type;
		HR(MFCreateMediaType(&Type));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
		HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, MediaFormatYUV));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
		HR(IMFMediaType_SetUINT32(Type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_RATE, MFT64(Config->FramerateNum, Config->FramerateDen)));
		HR(IMFMediaType_SetUINT64(Type, &MF_MT_FRAME_SIZE, MFT64(OutputWidth, OutputHeight)));

		hr = IMFSinkWriter_SetInputMediaType(Writer, Encoder->VideoStreamIndex, Type, NULL);
		IMFMediaType_Release(Type);

		if (FAILED(hr))
		{
			MessageBoxW(NULL, L"Cannot configure video encoder input!", WCAP_TITLE, MB_ICONERROR);
			goto bail;
		}
	}

	// video encoder parameters
	{
		ICodecAPI* Codec;
		HR(IMFSinkWriter_GetServiceForStream(Writer, 0, &GUID_NULL, &IID_ICodecAPI, (LPVOID*)&Codec));

		// VBR rate control
		VARIANT RateControl = { .vt = VT_UI4, .ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncCommonRateControlMode, &RateControl);

		// VBR bitrate to use, some MFT encoders override MF_MT_AVG_BITRATE setting with this one
		VARIANT Bitrate = { .vt = VT_UI4, .ulVal = Config->Config->VideoBitrate * 1000 };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncCommonMeanBitRate, &Bitrate);

		// set GOP size to 4 seconds
		VARIANT GopSize = { .vt = VT_UI4, .ulVal = MUL_DIV_ROUND_UP(4, Config->FramerateNum, Config->FramerateDen) };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVGOPSize, &GopSize);

		// disable low latency, for higher quality & better performance
		VARIANT LowLatency = { .vt = VT_BOOL, .boolVal = VARIANT_FALSE };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVLowLatencyMode, &LowLatency);

		// enable 2 B-frames, for better compression
		VARIANT Bframes = { .vt = VT_UI4, .ulVal = 2 };
		ICodecAPI_SetValue(Codec, &CODECAPI_AVEncMPVDefaultBPictureCount, &Bframes);

		ICodecAPI_Release(Codec);
	}

	if (Config->AudioFormat)
	{
		HR(CoCreateInstance(&CLSID_CResamplerMediaObject, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (LPVOID*)&Resampler));

		// audio resampler input
		{
			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(MFInitMediaTypeFromWaveFormatEx(Type, Config->AudioFormat, sizeof(*Config->AudioFormat) + Config->AudioFormat->cbSize));
			HR(IMFTransform_SetInputType(Resampler, 0, Type, 0));
			IMFMediaType_Release(Type);
		}

		// audio resampler output
		{
			WAVEFORMATEX Format =
			{
				.wFormatTag = WAVE_FORMAT_PCM,
				.nChannels = (WORD)Config->Config->AudioChannels,
				.nSamplesPerSec = Config->Config->AudioSamplerate,
				.wBitsPerSample = sizeof(short) * 8,
			};
			Format.nBlockAlign = Format.nChannels * Format.wBitsPerSample / 8;
			Format.nAvgBytesPerSec = Format.nSamplesPerSec * Format.nBlockAlign;

			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(MFInitMediaTypeFromWaveFormatEx(Type, &Format, sizeof(Format)));
			HR(IMFTransform_SetOutputType(Resampler, 0, Type, 0));
			IMFMediaType_Release(Type);
		}

		HR(IMFTransform_ProcessMessage(Resampler, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));

		// audio output type
		{
			const GUID* Codec = &((GUID[]){ MFAudioFormat_AAC, MFAudioFormat_FLAC })[Config->Config->AudioCodec];

			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, Codec));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, Config->Config->AudioSamplerate));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_NUM_CHANNELS, Config->Config->AudioChannels));
			if (Config->Config->AudioCodec == CONFIG_AUDIO_AAC)
			{
				HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, Config->Config->AudioBitrate * 1000 / 8));
			}

			hr = IMFSinkWriter_AddStream(Writer, Type, &Encoder->AudioStreamIndex);
			IMFMediaType_Release(Type);

			if (FAILED(hr))
			{
				MessageBoxW(NULL, L"Cannot configure audio encoder output!", WCAP_TITLE, MB_ICONERROR);
				goto bail;
			}
		}

		// audio input type
		{
			IMFMediaType* Type;
			HR(MFCreateMediaType(&Type));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio));
			HR(IMFMediaType_SetGUID(Type, &MF_MT_SUBTYPE, &MFAudioFormat_PCM));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, Config->Config->AudioSamplerate));
			HR(IMFMediaType_SetUINT32(Type, &MF_MT_AUDIO_NUM_CHANNELS, Config->Config->AudioChannels));

			hr = IMFSinkWriter_SetInputMediaType(Writer, Encoder->AudioStreamIndex, Type, NULL);
			IMFMediaType_Release(Type);

			if (FAILED(hr))
			{
				MessageBoxW(NULL, L"Cannot configure audio encoder input!", WCAP_TITLE, MB_ICONERROR);
				goto bail;
			}
		}
	}

	hr = IMFSinkWriter_BeginWriting(Writer);
	if (FAILED(hr))
	{
		MessageBoxW(NULL, L"Cannot start writing to mp4 file!", WCAP_TITLE, MB_ICONERROR);
		goto bail;
	}

	// input texture
	{
		TexResize_Create(&Encoder->Resize, Device, InputWidth, InputHeight, OutputWidth, OutputHeight, false, D3D11_BIND_RENDER_TARGET);

		D3D11_RENDER_TARGET_VIEW_DESC InputViewDesc =
		{
			.Format = DXGI_FORMAT_B8G8R8A8_UNORM,
			.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
		};
		ID3D11Device_CreateRenderTargetView(Device, (ID3D11Resource*)Encoder->Resize.InputTexture, &InputViewDesc, &Encoder->InputView);

		FLOAT Black[] = { 0, 0, 0, 0 };
		ID3D11DeviceContext_ClearRenderTargetView(Context, Encoder->InputView, Black);
	}

	// yuv converter
	{
		YuvConvert_Create(&Encoder->Convert, Device, Encoder->Resize.OutputTexture, OutputWidth, OutputHeight, ConvertFormat);

		UINT32 Size;
		HR(MFCalculateImageSize(MediaFormatYUV, OutputWidth, OutputHeight, &Size));

		for (size_t OutputIndex = 0; OutputIndex < ENCODER_VIDEO_BUFFER_COUNT; OutputIndex++)
		{
			YuvConvertOutput_Create(&Encoder->ConvertOutput[OutputIndex], Device, OutputWidth, OutputHeight, ConvertFormat);
			ID3D11Texture2D* ConvertOutputTexture = Encoder->ConvertOutput[OutputIndex].Texture;

			IMFSample* VideoSample;
			HR(MFCreateVideoSampleFromSurface(NULL, &VideoSample));

			IMFMediaBuffer* Buffer;
			HR(MFCreateDXGISurfaceBuffer(&IID_ID3D11Texture2D, (IUnknown*)ConvertOutputTexture, 0, FALSE, &Buffer));

			HR(IMFMediaBuffer_SetCurrentLength(Buffer, Size));
			HR(IMFSample_AddBuffer(VideoSample, Buffer));
			IMFMediaBuffer_Release(Buffer);

			Encoder->VideoSample[OutputIndex] = VideoSample;
		}
	}

	Encoder->InputWidth = Config->Width;
	Encoder->InputHeight = Config->Height;
	Encoder->OutputWidth = OutputWidth;
	Encoder->OutputHeight = OutputHeight;
	Encoder->FramerateNum = Config->FramerateNum;
	Encoder->FramerateDen = Config->FramerateDen;
	Encoder->VideoDiscontinuity = FALSE;
	Encoder->VideoLastTime = 0x8000000000000000ULL; // some large time in future
	Encoder->VideoIndex = 0;
	Encoder->VideoCount = ENCODER_VIDEO_BUFFER_COUNT;

	if (Encoder->AudioStreamIndex >= 0)
	{
		// resampler input buffer/sample
		{
			IMFSample* Sample;
			IMFMediaBuffer* Buffer;

			HR(MFCreateSample(&Sample));
			HR(MFCreateMemoryBuffer(Config->AudioFormat->nAvgBytesPerSec, &Buffer));
			HR(IMFSample_AddBuffer(Sample, Buffer));

			Encoder->AudioInputSample = Sample;
			Encoder->AudioInputBuffer = Buffer;
		}

		// resampler output & audio encoding input buffer/samples
		for (int i = 0; i < ENCODER_AUDIO_BUFFER_COUNT; i++)
		{
			IMFSample* Sample;
			IMFMediaBuffer* Buffer;
			IMFTrackedSample* Tracked;

			HR(MFCreateTrackedSample(&Tracked));
			HR(IMFTrackedSample_QueryInterface(Tracked, &IID_IMFSample, (LPVOID*)&Sample));
			HR(MFCreateMemoryBuffer(Config->Config->AudioSamplerate * Config->Config->AudioChannels * sizeof(short), &Buffer));
			HR(IMFSample_AddBuffer(Sample, Buffer));
			IMFMediaBuffer_Release(Buffer);
			IMFTrackedSample_Release(Tracked);

			Encoder->AudioSample[i] = Sample;
		}

		Encoder->AudioFrameSize = Config->AudioFormat->nBlockAlign;
		Encoder->AudioSampleRate = Config->AudioFormat->nSamplesPerSec;
		Encoder->Resampler = Resampler;
		Encoder->AudioIndex = 0;
		Encoder->AudioCount = ENCODER_AUDIO_BUFFER_COUNT;
	}

	ID3D11Device_AddRef(Device);
	ID3D11DeviceContext_AddRef(Context);
	Encoder->Context = Context;
	Encoder->Device = Device;

	Encoder->StartTime = 0;
	Encoder->Writer = Writer;
	Writer = NULL;
	Resampler = NULL;
	Result = TRUE;

bail:
	if (Resampler)
	{
		IMFTransform_Release(Resampler);
	}
	if (Writer)
	{
		IMFSinkWriter_Release(Writer);
		DeleteFileW(FileName);
	}
	ID3D11DeviceContext_Release(Context);
	IMFDXGIDeviceManager_Release(Manager);

	return Result;
}

void Encoder_Stop(Encoder* Encoder)
{
	if (Encoder->AudioStreamIndex >= 0)
	{
		HR(IMFTransform_ProcessMessage(Encoder->Resampler, MFT_MESSAGE_COMMAND_DRAIN, 0));
		Encoder__OutputAudioSamples(Encoder);
		IMFTransform_Release(Encoder->Resampler);
	}

	IMFSinkWriter_Finalize(Encoder->Writer);
	IMFSinkWriter_Release(Encoder->Writer);

	if (Encoder->AudioStreamIndex >= 0)
	{
		for (int i = 0; i < ENCODER_AUDIO_BUFFER_COUNT; i++)
		{
			IMFSample_Release(Encoder->AudioSample[i]);
		}
		IMFSample_Release(Encoder->AudioInputSample);
		IMFMediaBuffer_Release(Encoder->AudioInputBuffer);
	}

	for (size_t OutputIndex = 0; OutputIndex < ENCODER_VIDEO_BUFFER_COUNT; OutputIndex++)
	{
		YuvConvertOutput_Release(&Encoder->ConvertOutput[OutputIndex]);
		IMFSample_Release(Encoder->VideoSample[OutputIndex]);
	}
	YuvConvert_Release(&Encoder->Convert);
	TexResize_Release(&Encoder->Resize);
	ID3D11RenderTargetView_Release(Encoder->InputView);

	ID3D11DeviceContext_Release(Encoder->Context);
	ID3D11Device_Release(Encoder->Device);
}

BOOL Encoder_NewFrame(Encoder* Encoder, ID3D11Texture2D* Texture, RECT Rect, UINT64 Time, UINT64 TimePeriod)
{
	Encoder->VideoLastTime = Time;

	if (Encoder->VideoCount == 0)
	{
		// dropped frame
		LONGLONG Timestamp = MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0);
		HR(IMFSinkWriter_SendStreamTick(Encoder->Writer, Encoder->VideoStreamIndex, Timestamp));
		Encoder->VideoDiscontinuity = TRUE;
		return FALSE;
	}
	DWORD OutputIndex = Encoder->VideoIndex;
	Encoder->VideoIndex = (OutputIndex + 1) % ENCODER_VIDEO_BUFFER_COUNT;
	InterlockedDecrement(&Encoder->VideoCount);

	IMFSample* Sample = Encoder->VideoSample[OutputIndex];

	ID3D11DeviceContext* Context = Encoder->Context;

	// copy to input texture
	{
		D3D11_BOX Box =
		{
			.left = Rect.left,
			.top = Rect.top,
			.right = Rect.right,
			.bottom = Rect.bottom,
			.front = 0,
			.back = 1,
		};

		DWORD Width = Box.right - Box.left;
		DWORD Height = Box.bottom - Box.top;
		if (Width < Encoder->InputWidth || Height < Encoder->InputHeight)
		{
			FLOAT Black[] = { 0, 0, 0, 0 };
			ID3D11DeviceContext_ClearRenderTargetView(Context, Encoder->InputView, Black);

			Box.right = Box.left + min(Encoder->InputWidth, Box.right);
			Box.bottom = Box.top + min(Encoder->InputHeight, Box.bottom);
		}
		ID3D11DeviceContext_CopySubresourceRegion(Context, (ID3D11Resource*)Encoder->Resize.InputTexture, 0, 0, 0, 0, (ID3D11Resource*)Texture, 0, &Box);
	}

	// resize if needed
	TexResize_Dispatch(&Encoder->Resize, Context);

	// convert to YUV
	YuvConvert_Dispatch(&Encoder->Convert, Context, &Encoder->ConvertOutput[OutputIndex]);

	// setup input time & duration
	if (Encoder->StartTime == 0)
	{
		Encoder->StartTime = Time;
	}
	HR(IMFSample_SetSampleDuration(Sample, MFllMulDiv(Encoder->FramerateDen, MF_UNITS_PER_SECOND, Encoder->FramerateNum, 0)));
	HR(IMFSample_SetSampleTime(Sample, MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0)));

	if (Encoder->VideoDiscontinuity)
	{
		HR(IMFSample_SetUINT32(Sample, &MFSampleExtension_Discontinuity, TRUE));
		Encoder->VideoDiscontinuity = FALSE;
	}
	else
	{
		// don't care about success or no, we just don't want this attribute set at all
		IMFSample_DeleteItem(Sample, &MFSampleExtension_Discontinuity);
	}

	IMFTrackedSample* Tracked;
	HR(IMFSample_QueryInterface(Sample, &IID_IMFTrackedSample, (LPVOID*)&Tracked));
	IMFTrackedSample_SetAllocator(Tracked, &Encoder->VideoSampleCallback, NULL);
	IMFTrackedSample_Release(Tracked);

	// submit to encoder which will happen in background
	HR(IMFSinkWriter_WriteSample(Encoder->Writer, Encoder->VideoStreamIndex, Sample));

	IMFSample_Release(Sample);

	return TRUE;
}

void Encoder_NewSamples(Encoder* Encoder, LPCVOID Samples, DWORD VideoCount, UINT64 Time, UINT64 TimePeriod)
{
	IMFSample* AudioSample = Encoder->AudioInputSample;
	IMFMediaBuffer* Buffer = Encoder->AudioInputBuffer;

	BYTE* BufferData;
	DWORD MaxLength;
	HR(IMFMediaBuffer_Lock(Buffer, &BufferData, &MaxLength, NULL));

	DWORD BufferSize = VideoCount * Encoder->AudioFrameSize;
	Assert(BufferSize <= MaxLength);

	if (Samples)
	{
		CopyMemory(BufferData, Samples, BufferSize);
	}
	else
	{
		ZeroMemory(BufferData, BufferSize);
	}

	HR(IMFMediaBuffer_Unlock(Buffer));
	HR(IMFMediaBuffer_SetCurrentLength(Buffer, BufferSize));

	// setup input time & duration
	Assert(Encoder->StartTime != 0);
	HR(IMFSample_SetSampleDuration(AudioSample, MFllMulDiv(VideoCount, MF_UNITS_PER_SECOND, Encoder->AudioSampleRate, 0)));
	HR(IMFSample_SetSampleTime(AudioSample, MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0)));

	HR(IMFTransform_ProcessInput(Encoder->Resampler, 0, AudioSample, 0));
	Encoder__OutputAudioSamples(Encoder);
}

void Encoder_Update(Encoder* Encoder, UINT64 Time, UINT64 TimePeriod)
{
	// if there was no frame during last second, add discontinuity
	if (Time - Encoder->VideoLastTime >= TimePeriod)
	{
		Encoder->VideoLastTime = Time;
		LONGLONG Timestamp = MFllMulDiv(Time - Encoder->StartTime, MF_UNITS_PER_SECOND, TimePeriod, 0);
		HR(IMFSinkWriter_SendStreamTick(Encoder->Writer, Encoder->VideoStreamIndex, Timestamp));
		Encoder->VideoDiscontinuity = TRUE;
	}
}

void Encoder_GetStats(Encoder* Encoder, DWORD* Bitrate, DWORD* LengthMsec, UINT64* FileSize)
{
	MF_SINK_WRITER_STATISTICS Stats = { .cb = sizeof(Stats) };
	HR(IMFSinkWriter_GetStatistics(Encoder->Writer, Encoder->VideoStreamIndex, &Stats));

	*Bitrate = (DWORD)MFllMulDiv(8 * Stats.qwByteCountProcessed, MF_UNITS_PER_SECOND, 1000 * Stats.llLastTimestampProcessed, 0);
	*LengthMsec = (DWORD)(Stats.llLastTimestampProcessed / 10000);
	*FileSize = Stats.qwByteCountProcessed;

	if (Encoder->AudioStreamIndex >= 0)
	{
		HR(IMFSinkWriter_GetStatistics(Encoder->Writer, Encoder->AudioStreamIndex, &Stats));
		*Bitrate += (DWORD)MFllMulDiv(8 * Stats.qwByteCountProcessed, MF_UNITS_PER_SECOND, 1000 * Stats.llLastTimestampProcessed, 0);
		*FileSize += Stats.qwByteCountProcessed;
	}
}
