// Copyright (c) 2013- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <cmath>

#include "Common/Serialize/SerializeFuncs.h"
#include "Core/Debugger/MemBlockInfo.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HW/SimpleAudioDec.h"
#include "Core/HW/MediaEngine.h"
#include "Core/HW/BufferQueue.h"
#include "Core/HW/Atrac3Standalone.h"

#include "ext/minimp3/minimp3.h"

#ifdef USE_FFMPEG

extern "C" {
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "libavutil/samplefmt.h"
#include "libavcodec/avcodec.h"
#include "libavutil/version.h"

#include "Core/FFMPEGCompat.h"
}
#include "Core/Config.h"

#else

extern "C" {
	struct AVCodec;
	struct AVCodecContext;
	struct SwrContext;
	struct AVFrame;
}

#endif  // USE_FFMPEG

// AAC decoder candidates:
// * https://github.com/mstorsjo/fdk-aac/tree/master

// h.264 decoder candidates:
// * https://github.com/meerkat-cv/h264_decoder
// * https://github.com/shengbinmeng/ffmpeg-h264-dec

// minimp3-based decoder.
class MiniMp3Audio : public AudioDecoder {
public:
	MiniMp3Audio() {
		mp3dec_init(&mp3_);
	}
	~MiniMp3Audio() {}

	bool Decode(const uint8_t* inbuf, int inbytes, int *inbytesConsumed, int outputChannels, int16_t *outbuf, int *outSamples) override {
		_dbg_assert_(outputChannels == 2);

		// When used from sceMp3LowLevelDecode, this fails to parse the mp3 header!
		// It's because minimp3 is a bit more sensitive than ffmpeg - if you give it a buffer that's larger than the frame size,
		// it'll check that there's a second matching frame before accepting. But in our case we only get one frame,
		// but we do not know the size. So this might need some modifications in minimp3.
		mp3dec_frame_info_t info{};
		int samplesWritten = mp3dec_decode_frame(&mp3_, inbuf, inbytes, (mp3d_sample_t *)temp_, &info);
		_dbg_assert_(samplesWritten <= MINIMP3_MAX_SAMPLES_PER_FRAME);
		_dbg_assert_(info.channels <= 2);
		if (info.channels == 1) {
			for (int i = 0; i < samplesWritten; i++) {
				outbuf[i * 2] = temp_[i];
				outbuf[i * 2 + 1] = temp_[i];
			}
		} else {
			memcpy(outbuf, temp_, 4 * samplesWritten);
		}
		*inbytesConsumed = info.frame_bytes;
		*outSamples = samplesWritten;
		return true;
	}

	bool IsOK() const override { return true; }
	void SetChannels(int channels) override {
		// Hmm. ignore for now.
	}

	PSPAudioType GetAudioType() const override { return PSP_CODEC_MP3; }

private:
	// We use the lowest-level API.
	mp3dec_t mp3_{};
	int16_t temp_[MINIMP3_MAX_SAMPLES_PER_FRAME]{};
};

// FFMPEG-based decoder. TODO: Replace with individual codecs.
// Based on http://ffmpeg.org/doxygen/trunk/doc_2examples_2decoding_encoding_8c-example.html#_a13
class FFmpegAudioDecoder : public AudioDecoder {
public:
	FFmpegAudioDecoder(PSPAudioType audioType, int sampleRateHz = 44100, int channels = 2);
	~FFmpegAudioDecoder();

	bool Decode(const uint8_t* inbuf, int inbytes, int *inbytesConsumed, int outputChannels, int16_t *outbuf, int *outSamples) override;
	bool IsOK() const override {
#ifdef USE_FFMPEG
		return codec_ != 0;
#else
		return 0;
#endif
	}

	void SetChannels(int channels) override;
	void FlushBuffers() override;

	// These two are only here because of save states.
	PSPAudioType GetAudioType() const override { return audioType; }

private:
	bool OpenCodec(int block_align);

	PSPAudioType audioType;
	int sample_rate_;
	int channels_;

	AVFrame *frame_ = nullptr;
	AVCodec *codec_ = nullptr;
	AVCodecContext  *codecCtx_ = nullptr;
#ifdef USE_FFMPEG
	AVCodecParserContext *parser_ = nullptr;
#endif
	SwrContext      *swrCtx_ = nullptr;

	bool codecOpen_ = false;
};

AudioDecoder *CreateAudioDecoder(PSPAudioType audioType, int sampleRateHz, int channels, size_t blockAlign, const uint8_t *extraData, size_t extraDataSize) {
	bool forceFfmpeg = false;
#ifdef USE_FFMPEG
	forceFfmpeg = g_Config.bForceFfmpegForAudioDec;
#endif
	if (forceFfmpeg) {
		return new FFmpegAudioDecoder(audioType, sampleRateHz, channels);
	}

	switch (audioType) {
	// Our MiniMP3 backend has too many issues:
	//   * Doesn't accept sample rate
	//   * Doesn't accept data where there's only one valid frame if the buffer is bigger.
	//     This prevents sceMp3LowLevelDecode from working, since nothing passes us the frame size.
	//
	// case PSP_CODEC_MP3:
	// 	return new MiniMp3Audio();
	case PSP_CODEC_AT3:
	 	return CreateAtrac3Audio(channels, blockAlign, extraData, extraDataSize);
	case PSP_CODEC_AT3PLUS:
		return CreateAtrac3PlusAudio(channels, blockAlign);
	default:
		// Only AAC normally falls back to FFMPEG now.
		return new FFmpegAudioDecoder(audioType, sampleRateHz, channels);
	}
}

static int GetAudioCodecID(int audioType) {
#ifdef USE_FFMPEG
	switch (audioType) {
	case PSP_CODEC_AAC:
		return AV_CODEC_ID_AAC;
	case PSP_CODEC_AT3:
		return AV_CODEC_ID_ATRAC3;
	case PSP_CODEC_AT3PLUS:
		return AV_CODEC_ID_ATRAC3P;
	case PSP_CODEC_MP3:
		return AV_CODEC_ID_MP3;
	default:
		return AV_CODEC_ID_NONE;
	}
#else
	return 0;
#endif // USE_FFMPEG
}

FFmpegAudioDecoder::FFmpegAudioDecoder(PSPAudioType audioType, int sampleRateHz, int channels)
	: audioType(audioType), sample_rate_(sampleRateHz), channels_(channels) {

#ifdef USE_FFMPEG
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 18, 100)
	avcodec_register_all();
#endif
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 12, 100)
	av_register_all();
#endif
	InitFFmpeg();

	frame_ = av_frame_alloc();

	// Get AUDIO Codec ctx
	int audioCodecId = GetAudioCodecID(audioType);
	if (!audioCodecId) {
		ERROR_LOG(Log::ME, "This version of FFMPEG does not support Audio codec type: %08x. Update your submodule.", audioType);
		return;
	}
	// Find decoder
	codec_ = avcodec_find_decoder((AVCodecID)audioCodecId);
	if (!codec_) {
		// Eh, we shouldn't even have managed to compile. But meh.
		ERROR_LOG(Log::ME, "This version of FFMPEG does not support AV_CODEC_ctx for audio (%s). Update your submodule.", GetCodecName(audioType));
		return;
	}
	// Allocate codec context
	codecCtx_ = avcodec_alloc_context3(codec_);
	if (!codecCtx_) {
		ERROR_LOG(Log::ME, "Found a decoder for audio codec ID %08x but failed to allocate a codec context. Strange.", audioCodecId);
		return;
	}
#if LIBAVUTIL_VERSION_MAJOR >= 59
	if (channels_ == 2)
		codecCtx_->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
	else
		codecCtx_->ch_layout = AV_CHANNEL_LAYOUT_MONO;
#else
	codecCtx_->channels = channels_;
	codecCtx_->channel_layout = channels_ == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
#endif
	codecCtx_->sample_rate = sample_rate_;
#endif  // USE_FFMPEG
}

bool FFmpegAudioDecoder::OpenCodec(int block_align) {
#ifdef USE_FFMPEG
	if (!codec_ || !codecCtx_) {
		ERROR_LOG(Log::ME, "Codec context not allocated for some reason. This is bad.");
		return false;
	}
	// Some versions of FFmpeg require this set. It may also be set in SetExtraData().
	if (codecCtx_->block_align == 0) {
		codecCtx_->block_align = block_align;
	}
	if (audioType == PSP_CODEC_MP3 && !parser_) {
		parser_ = av_parser_init(AV_CODEC_ID_MP3);
		if (!parser_) {
			ERROR_LOG(Log::ME, "Failed to allocate the MP3 parser");
			return false;
		}
	}

	AVDictionary *opts = 0;
	int retval = avcodec_open2(codecCtx_, codec_, &opts);
	if (retval < 0) {
		ERROR_LOG(Log::ME, "Failed to open codec: retval = %i", retval);
	}
	av_dict_free(&opts);
	codecOpen_ = retval >= 0;
	return retval >= 0;
#else
	return false;
#endif  // USE_FFMPEG
}

void FFmpegAudioDecoder::SetChannels(int channels) {
	if (channels_ == channels) {
		// Do nothing, already set.
		return;
	}
#ifdef USE_FFMPEG

	if (codecOpen_) {
		ERROR_LOG(Log::ME, "Codec already open, cannot change channels");
	} else {
		channels_ = channels;
#if LIBAVUTIL_VERSION_MAJOR >= 59
		if (channels_ == 2)
			codecCtx_->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
		else
			codecCtx_->ch_layout = AV_CHANNEL_LAYOUT_MONO;
#else
		codecCtx_->channels = channels_;
		codecCtx_->channel_layout = channels_ == 2 ? AV_CH_LAYOUT_STEREO : AV_CH_LAYOUT_MONO;
#endif
	}
#endif
}

void FFmpegAudioDecoder::FlushBuffers() {
#ifdef USE_FFMPEG
	if (codecCtx_ && codecOpen_)
		avcodec_flush_buffers(codecCtx_);
	if (parser_) {
		av_parser_close(parser_);
		parser_ = av_parser_init(AV_CODEC_ID_MP3);
	}
	swr_free(&swrCtx_);
#endif
}

FFmpegAudioDecoder::~FFmpegAudioDecoder() {
#ifdef USE_FFMPEG
	swr_free(&swrCtx_);
	if (parser_)
		av_parser_close(parser_);
	av_frame_free(&frame_);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(55, 52, 0)
	avcodec_free_context(&codecCtx_);
#else
	// Future versions may add other things to free, but avcodec_free_context didn't exist yet here.
	avcodec_close(codecCtx_);
	av_freep(&codecCtx_->extradata);
	av_freep(&codecCtx_->subtitle_header);
	av_freep(&codecCtx_);
#endif
	codec_ = 0;
#endif  // USE_FFMPEG
}

// Decodes available input, producing at most one output frame.
bool FFmpegAudioDecoder::Decode(const uint8_t *inbuf, int inbytes, int *inbytesConsumed, int outputChannels, int16_t *outbuf, int *outSamples) {
#ifdef USE_FFMPEG
	if (!codecOpen_) {
		OpenCodec(inbytes);
		if (!codecOpen_) {
			ERROR_LOG(Log::ME, "Codec not open, can't decode.");
			return false;
		}
	}

	AVPacket packet;
	av_init_packet(&packet);
	const uint8_t *packetData = inbuf;
	int packetSize = inbytes;
	int inputConsumed = 0;

	if (parser_) {
		uint8_t *parsedData = nullptr;
		int parsedSize = 0;
		inputConsumed = av_parser_parse2(parser_, codecCtx_, &parsedData, &parsedSize,
			inbuf, inbytes, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
		if (inputConsumed < 0) {
			ERROR_LOG(Log::ME, "Error parsing MP3 frame (%d bytes): %d", inbytes, inputConsumed);
			return false;
		}
		packetData = parsedData;
		packetSize = parsedSize;
	}
	packet.data = (uint8_t *)(packetData);
	packet.size = packetSize;

	int got_frame = 0;
	av_frame_unref(frame_);

	if (outSamples) {
		*outSamples = 0;
	}
	if (inbytesConsumed) {
		*inbytesConsumed = 0;
	}
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	if (packetSize != 0) {
		int err = avcodec_send_packet(codecCtx_, &packet);
		if (err < 0) {
			ERROR_LOG(Log::ME, "Error sending audio frame to decoder (%d bytes): %d (%08x)", inbytes, err, err);
			return false;
		}
	}
	int err = avcodec_receive_frame(codecCtx_, frame_);
	int len = 0;
	if (err >= 0) {
		len = parser_ ? inputConsumed : packet.size;
		got_frame = 1;
	} else if (err != AVERROR(EAGAIN)) {
		len = err;
	} else {
		// The parser may have consumed a partial frame without producing a packet.
		// Those input bytes still belong to the parser and must be removed by the caller.
		len = inputConsumed;
	}
#else
	int decoderConsumed = avcodec_decode_audio4(codecCtx_, frame_, &got_frame, &packet);
	int len = parser_ ? inputConsumed : decoderConsumed;
#endif
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 12, 100)
	av_packet_unref(&packet);
#else
	av_free_packet(&packet);
#endif

	if (len < 0) {
		ERROR_LOG(Log::ME, "Error decoding Audio frame (%i bytes): %i (%08x)", inbytes, len, len);
		return false;
	}
	
	// get bytes consumed in source
	if (inbytesConsumed) {
		*inbytesConsumed = len;
	}

	if (got_frame) {
		// Initializing the sample rate convert. We will use it to convert float output into int.
		_dbg_assert_(outputChannels == 2);
#if LIBAVUTIL_VERSION_MAJOR >= 59
		AVChannelLayout wanted_channel_layout = AV_CHANNEL_LAYOUT_STEREO; // we want stereo output layout
		const AVChannelLayout& dec_channel_layout = frame_->ch_layout; // decoded channel layout
#else
		int64_t wanted_channel_layout = AV_CH_LAYOUT_STEREO; // we want stereo output layout
		int64_t dec_channel_layout = frame_->channel_layout; // decoded channel layout
#endif

		if (!swrCtx_) {
			// TODO: Allow these to differ.
			const int inputSampleRate = codecCtx_->sample_rate;
			const int outputSampleRate = codecCtx_->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 59
			swr_alloc_set_opts2(
				&swrCtx_,
				&wanted_channel_layout,
				AV_SAMPLE_FMT_S16,
				outputSampleRate,
				&dec_channel_layout,
				codecCtx_->sample_fmt,
				inputSampleRate,
				0,
				NULL);
#else
			swrCtx_ = swr_alloc_set_opts(
				swrCtx_,
				wanted_channel_layout,
				AV_SAMPLE_FMT_S16,
				outputSampleRate,
				dec_channel_layout,
				codecCtx_->sample_fmt,
				inputSampleRate,
				0,
				NULL);
#endif

			if (!swrCtx_ || swr_init(swrCtx_) < 0) {
				ERROR_LOG(Log::ME, "swr_init: Failed to initialize the resampling context");
#if LIBAVCODEC_VERSION_MAJOR >= 62
				avcodec_free_context(&codecCtx_);
#else
				avcodec_close(codecCtx_);
#endif
				codec_ = 0;
				return false;
			}
		}

		// convert audio to AV_SAMPLE_FMT_S16
		int swrRet = 0;
		if (outbuf != nullptr) {
			swrRet = swr_convert(swrCtx_, (uint8_t **)&outbuf, frame_->nb_samples, (const u8 **)frame_->extended_data, frame_->nb_samples);
		}
		if (swrRet < 0) {
			ERROR_LOG(Log::ME, "swr_convert: Error while converting: %d", swrRet);
			return false;
		}
		// output stereo samples per frame
		if (outSamples) {
			*outSamples = swrRet;
		}

		// Save outbuf into pcm audio, you can uncomment this line to save and check the decoded audio into pcm file.
		// SaveAudio("dump.pcm", outbuf, *outbytes);
	}
	return true;
#else
	// Zero bytes output. No need to memset.
	*outSamples = 0;
	return true;
#endif  // USE_FFMPEG
}

void AudioClose(AudioDecoder **ctx) {
	delete *ctx;
	*ctx = 0;
}

void AudioClose(FFmpegAudioDecoder **ctx) {
#ifdef USE_FFMPEG
	delete *ctx;
	*ctx = 0;
#endif  // USE_FFMPEG
}

static const char *const codecNames[4] = {
	"AT3+", "AT3", "MP3", "AAC",
};

static bool IsPlausibleMp3Header(const u8 *header) {
	if (header[0] != 0xFF || (header[1] & 0xC0) != 0xC0)
		return false;
	const int version = (header[1] >> 3) & 0x3;
	const int layer = (header[1] >> 1) & 0x3;
	const int bitrate = (header[2] >> 4) & 0xF;
	const int sampleRate = (header[2] >> 2) & 0x3;
	return version != 1 && layer == 1 && bitrate != 0 && bitrate != 15 && sampleRate != 3;
}

const char *GetCodecName(int codec) {
	if (codec >= PSP_CODEC_AT3PLUS && codec <= PSP_CODEC_AAC) {
		return codecNames[codec - PSP_CODEC_AT3PLUS];
	} else {
		return "(unk)";
	}
};

bool IsValidCodec(PSPAudioType codec){
	if (codec >= PSP_CODEC_AT3PLUS && codec <= PSP_CODEC_AAC) {
		return true;
	}
	return false;
}


// sceAu module starts from here

AuCtx::AuCtx() {
}

AuCtx::~AuCtx() {
	if (decoder) {
		AudioClose(&decoder);
		decoder = nullptr;
	}
}

size_t AuCtx::FindNextMp3Sync() {
	// The scan reads the current byte and the following byte.
	if (sourcebuff.size() < 2) {
		return 0;
	}
	for (size_t i = 0; i + 1 < sourcebuff.size(); ++i) {
		if (i + 3 < sourcebuff.size() && IsPlausibleMp3Header(&sourcebuff[i])) {
			return i;
		}
	}
	return 0;
}

// Return the number of output PCM bytes, or -1 if decoding fails.
int AuCtx::AuDecode(u32 pcmAddr) {
	u32 outptr = PCMBuf + nextOutputHalf * PCMBufSize / 2;
	auto outbuf = Memory::GetPointerWriteRangeOrException(outptr, PCMBufSize / 2);
	int outpcmbufsize = 0;

	if (pcmAddr)
		Memory::WriteOrException_U32(outptr, pcmAddr);

	// Decode available input from sourcebuff and output at most one frame into PCMBuf.
	if (!sourcebuff.empty()) {
		// Align the input to an MP3 sync before passing it to the decoder.
		int nextSync = 0;
		if (decoder->GetAudioType() == PSP_CODEC_MP3) {
			nextSync = (int)FindNextMp3Sync();
		}
		int inbytesConsumed = 0;
		int outSamples = 0;
		bool decoded = decoder->Decode(&sourcebuff[nextSync], (int)sourcebuff.size() - nextSync, &inbytesConsumed, 2, (int16_t *)outbuf, &outSamples);
		outpcmbufsize = outSamples * 2 * sizeof(int16_t);
		int srcPos = inbytesConsumed + nextSync;
		if (srcPos > 0 && srcPos <= (int)sourcebuff.size()) {
			sourcebuff.erase(sourcebuff.begin(), sourcebuff.begin() + srcPos);
			AuBufAvailable -= srcPos;
		}

		if (!decoded) {
			return -1;
		} else if (outpcmbufsize == 0 && sourcebuff.empty()) {
			// Nothing was output, hopefully we're at the end of the stream.
			// Keep parser state intact: it may contain a partial frame.
		} else {
			// Count decoded samples, but not the stereo channel multiplier.
			SumDecodedSamples += outSamples;
		}
	}

	bool end = endPos && readPos - AuBufAvailable >= (int64_t)endPos;
	if (end && LoopNum != 0) {
		// When looping, reset to the start position and clear stale buffer data.
		// If we don't clear sourcebuff and AuBufAvailable, stale compressed data remains
		// and gets decoded as part of the loop, causing games like Crazy Taxi to repeat
		// a small section instead of looping the full track.
		SumDecodedSamples = 0;
		readPos = startPos;
		AuBufAvailable = 0;
		sourcebuff.clear();
		decoder->FlushBuffers();
		if (LoopNum > 0)
			LoopNum--;
	}

	if (outpcmbufsize == 0 && !end) {
		// If we didn't decode anything, we fill this half of the buffer with zeros.
		outpcmbufsize = PCMBufSize / 2;
		if (outbuf != nullptr)
			memset(outbuf, 0, outpcmbufsize);
	} else if ((u32)outpcmbufsize < PCMBufSize / 2) {
		if (outbuf != nullptr)
			memset((u8 *)outbuf + outpcmbufsize, 0, PCMBufSize / 2 - outpcmbufsize);
	}

	if (outpcmbufsize != 0)
		NotifyMemInfo(MemBlockFlags::WRITE, outptr, outpcmbufsize, "AuDecode");

	nextOutputHalf ^= 1;
	return outpcmbufsize;
}

// return 1 to read more data stream, 0 don't read
int AuCtx::AuCheckStreamDataNeeded() {
	// If we would ask for bytes, then some are needed.
	if (AuStreamBytesNeeded() > 0) {
		return 1;
	}
	return 0;
}

int AuCtx::AuStreamBytesNeeded() {
	if (decoder->GetAudioType() == PSP_CODEC_MP3) {
		// endPos is zero for handles whose stream range is supplied later.
		if (endPos && readPos >= endPos)
            return 0;

		// Request bounded chunks so startup and track changes do not wait for the
		// entire input buffer to be filled.
		int offset = AuStreamWorkareaSize();
		int spaceFree = (int)AuBufSize - offset - AuBufAvailable;

		// Stop only when the usable buffer is full.
		if (spaceFree <= 0)
            return 0;

		int remaining = spaceFree;
		if (endPos)
			remaining = (int)std::min<u64>(endPos - readPos, INT_MAX);

		int requestedSize = 0;

		// Use a larger request while initially filling the payload buffer.
		if (AuBufAvailable < MP3_STREAMING_CHUNK_INITIAL) {
            requestedSize = std::min(MP3_STREAMING_CHUNK_INITIAL, spaceFree);
        } else {
			// Once the initial buffer is filled, use smaller requests to top it off.
            requestedSize = std::min(MP3_STREAMING_CHUNK_ONGOING, spaceFree);
        }

		// Ensure we don't request more than what is left in an initialized stream.
		if (endPos)
			requestedSize = std::min(requestedSize, remaining);

		// Avoid tiny refill requests once the buffer is initially filled, but allow
		// a short final read needed to reach endPos.
		if (AuBufAvailable >= MP3_STREAMING_CHUNK_INITIAL &&
			requestedSize < MP3_STREAMING_MIN_SPACE && remaining > requestedSize)
			return 0;
		return requestedSize;
    }

	// TODO: Untested.  Maybe similar to MP3.
	return std::min<int64_t>((int)AuBufSize - AuBufAvailable, (int64_t)endPos - readPos);
}

int AuCtx::AuStreamWorkareaSize() {
	// Note that this is 32 bytes more than the max layer 3 frame size.
	if (decoder->GetAudioType() == PSP_CODEC_MP3)
		return 0x05c0;
	return 0;
}

// check how many bytes we have read from source file
u32 AuCtx::AuNotifyAddStreamData(int size) {
	int offset = AuStreamWorkareaSize();

	// `size` is game-supplied and was previously trusted outright: a negative value
	// would make sourcebuff.resize() attempt a huge allocation (size_t underflow),
	// and an unbounded positive value would grow sourcebuff without limit (DoS).
	// The validated range also has to match what's actually read below - it was
	// checking [AuBuf, AuBuf+size) while the copy reads from [AuBuf+offset, ...).
	// Only update counters if data is actually valid and can be copied!
	int spaceFree = (int)AuBufSize - offset - AuBufAvailable;
	if (askedReadSize != 0 && size > askedReadSize)
		return (u32)-1;
	if (endPos && readPos <= (int64_t)endPos && size > (int64_t)endPos - readPos)
		return (u32)-1;
	if (offset <= AuBufSize && size > 0 && size <= spaceFree && Memory::IsValidRange(AuBuf + offset, size)) {
		sourcebuff.resize(sourcebuff.size() + size);
		Memory::MemcpyUnchecked(&sourcebuff[sourcebuff.size() - size], AuBuf + offset, size);

		// Only update tracking after successful copy
		readPos += size;
		AuBufAvailable += size;
		askedReadSize = 0;
	} else {
		return (u32)-1;
	}

	return 0;
}

// read from stream position srcPos of size bytes into buff
// buff, size and srcPos are all pointers
u32 AuCtx::AuGetInfoToAddStreamData(u32 bufPtr, u32 sizePtr, u32 srcPosPtr) {
	int readsize = AuStreamBytesNeeded();
	int offset = AuStreamWorkareaSize();

	// we can recharge AuBuf from its beginning
	if (readsize != 0) {
		if (Memory::IsValidAddress(bufPtr))
			Memory::WriteUnchecked_U32(AuBuf + offset, bufPtr);
		if (Memory::IsValidAddress(sizePtr))
			Memory::WriteUnchecked_U32(readsize, sizePtr);
		if (Memory::IsValidAddress(srcPosPtr))
			Memory::WriteUnchecked_U32(readPos, srcPosPtr);
	} else {
		if (Memory::IsValidAddress(bufPtr))
			Memory::WriteUnchecked_U32(0, bufPtr);
		if (Memory::IsValidAddress(sizePtr))
			Memory::WriteUnchecked_U32(0, sizePtr);
		if (Memory::IsValidAddress(srcPosPtr))
			Memory::WriteUnchecked_U32(0, srcPosPtr);
	}

	// Keep the request size so notification can reject an oversized read.
	askedReadSize = readsize;
	return 0;
}

u32 AuCtx::AuResetPlayPositionByFrame(int frame) {
	// Note: this doesn't correctly handle padding or slot size, but the PSP doesn't either.
	int64_t bytesPerSecond = (int64_t)(MaxOutputSample / 8) * BitRate * 1000;
	readPos = (int64_t)startPos + ((int64_t)frame * bytesPerSecond) / SamplingRate;
	// Not sure why, but it seems to consistently seek 1 before, maybe in case it's off slightly.
	if (frame != 0)
		readPos -= 1;
	SumDecodedSamples = (u32)((int64_t)frame * MaxOutputSample);
	AuBufAvailable = 0;
	sourcebuff.clear();
	decoder->FlushBuffers();
	return 0;
}

u32 AuCtx::AuResetPlayPosition() {
	readPos = startPos;
	SumDecodedSamples = 0;
	AuBufAvailable = 0;
	sourcebuff.clear();
	decoder->FlushBuffers();
	return 0;
}

void AuCtx::DoState(PointerWrap &p) {
	auto s = p.Section("AuContext", 0, 2);
	if (!s)
		return;

	Do(p, startPos);
	Do(p, endPos);
	Do(p, AuBuf);
	Do(p, AuBufSize);
	Do(p, PCMBuf);
	Do(p, PCMBufSize);
	Do(p, freq);
	Do(p, SumDecodedSamples);
	Do(p, LoopNum);
	Do(p, Channels);
	Do(p, MaxOutputSample);
	Do(p, readPos);
	int audioType = decoder ? (int)decoder->GetAudioType() : 0;
	Do(p, audioType);
	Do(p, BitRate);
	Do(p, SamplingRate);
	Do(p, askedReadSize);
	int dummy = 0;
	Do(p, dummy);
	Do(p, FrameNum);

	if (s < 2) {
		AuBufAvailable = 0;
		Version = 3;
	} else {
		Do(p, Version);
		Do(p, AuBufAvailable);
		Do(p, sourcebuff);
		Do(p, nextOutputHalf);
	}

	if (p.mode == p.MODE_READ) {
		// FFmpeg parser/codec internals are not serialized; recreate the decoder
		// with the saved stream format and serialized source-buffer state.
		decoder = CreateAudioDecoder((PSPAudioType)audioType,
			SamplingRate > 0 ? SamplingRate : 44100,
			Channels > 0 ? Channels : 2);
	}
}
