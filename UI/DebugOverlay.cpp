#include "Common/Render/DrawBuffer.h"
#include "Common/GPU/thin3d.h"
#include "Common/System/System.h"
#include "Common/Data/Text/I18n.h"
#include "Common/CPUDetect.h"
#include "Common/StringUtils.h"
#include "Common/Data/Text/Parsers.h"

#include "Core/MIPS/MIPS.h"
#include "Core/HW/Display.h"
#include "Core/FrameTiming.h"
#include "Core/HLE/sceSas.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/scePower.h"
#include "Core/HLE/Plugins.h"
#include "Core/ControlMapper.h"
#include "Core/Config.h"
#include "Core/MemFault.h"
#include "Core/Reporting.h"
#include "Core/CwCheat.h"
#include "Core/Core.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/System.h"
#include "Core/Util/GameDB.h"
#include "GPU/GPU.h"
#include "GPU/GPUCommon.h"
// TODO: This should be moved here or to Common, doesn't belong in /GPU
#include "GPU/Vulkan/DebugVisVulkan.h"
#include "GPU/Common/FramebufferManagerCommon.h"

#include "UI/DevScreens.h"
#include "UI/DebugOverlay.h"

// For std::max
#include <algorithm>

static void DrawDebugStats(UIContext *ctx, const Bounds &bounds) {
	FontID ubuntu24("UBUNTU24");

	float left = std::max(bounds.w / 2 - 20.0f, 550.0f);
	float right = bounds.w - left - 20.0f;

	char statbuf[4096];

	ctx->Flush();
	ctx->BindFontTexture();
	ctx->Draw()->SetFontScale(.7f, .7f);

	__DisplayGetDebugStats(statbuf, sizeof(statbuf));
	ctx->Draw()->DrawTextRect(ubuntu24, statbuf, bounds.x + 11, bounds.y + 31, left, bounds.h - 30, 0xc0000000, FLAG_DYNAMIC_ASCII);
	ctx->Draw()->DrawTextRect(ubuntu24, statbuf, bounds.x + 10, bounds.y + 30, left, bounds.h - 30, 0xFFFFFFFF, FLAG_DYNAMIC_ASCII);

	ctx->Draw()->SetFontScale(1.0f, 1.0f);
	ctx->Flush();
	ctx->RebindTexture();
}

static void DrawAudioDebugStats(UIContext *ctx, const Bounds &bounds) {
	FontID ubuntu24("UBUNTU24");

	char statbuf[4096] = { 0 };
	System_AudioGetDebugStats(statbuf, sizeof(statbuf));

	ctx->Flush();
	ctx->BindFontTexture();
	ctx->Draw()->SetFontScale(0.5f, 0.5f);
	ctx->Draw()->DrawTextRect(ubuntu24, statbuf, bounds.x + 11, bounds.y + 31, bounds.w - 20, bounds.h - 30, 0xc0000000, FLAG_DYNAMIC_ASCII);
	ctx->Draw()->DrawTextRect(ubuntu24, statbuf, bounds.x + 10, bounds.y + 30, bounds.w - 20, bounds.h - 30, 0xFFFFFFFF, FLAG_DYNAMIC_ASCII);

	float left = std::max(bounds.w / 2 - 20.0f, 500.0f);

	__SasGetDebugStats(statbuf, sizeof(statbuf));
	ctx->Draw()->DrawTextRect(ubuntu24, statbuf, bounds.x + left + 21, bounds.y + 31, bounds.w - left, bounds.h - 30, 0xc0000000, FLAG_DYNAMIC_ASCII);
	ctx->Draw()->DrawTextRect(ubuntu24, statbuf, bounds.x + left + 20, bounds.y + 30, bounds.w - left, bounds.h - 30, 0xFFFFFFFF, FLAG_DYNAMIC_ASCII);

	ctx->Draw()->SetFontScale(1.0f, 1.0f);

	ctx->Flush();
	ctx->RebindTexture();
}

static void DrawControlDebug(UIContext *ctx, const ControlMapper &mapper, const Bounds &bounds) {
	FontID ubuntu24("UBUNTU24");

	char statbuf[4096] = { 0 };
	mapper.GetDebugString(statbuf, sizeof(statbuf));

	ctx->Flush();
	ctx->BindFontTexture();
	ctx->Draw()->SetFontScale(0.5f, 0.5f);
	ctx->Draw()->DrawTextRect(ubuntu24, statbuf, bounds.x + 11, bounds.y + 31, bounds.w - 20, bounds.h - 30, 0xc0000000, FLAG_DYNAMIC_ASCII);
	ctx->Draw()->DrawTextRect(ubuntu24, statbuf, bounds.x + 10, bounds.y + 30, bounds.w - 20, bounds.h - 30, 0xFFFFFFFF, FLAG_DYNAMIC_ASCII);
	ctx->Draw()->SetFontScale(1.0f, 1.0f);
	ctx->Flush();
	ctx->RebindTexture();
}

static void DrawFrameTimes(UIContext *ctx, const Bounds &bounds) {
	FontID ubuntu24("UBUNTU24");
	double *sleepHistory;
	int valid, pos;
	double *history = __DisplayGetFrameTimes(&valid, &pos, &sleepHistory);
	int scale = 7000;
	int width = 600;

	ctx->Flush();
	ctx->BeginNoTex();
	int bottom = bounds.y2();
	for (int i = 0; i < valid; ++i) {
		double activeTime = history[i] - sleepHistory[i];
		ctx->Draw()->vLine(bounds.x + i, bottom, bottom - activeTime * scale, 0xFF3FFF3F);
		ctx->Draw()->vLine(bounds.x + i, bottom - activeTime * scale, bottom - history[i] * scale, 0x7F3FFF3F);
	}
	ctx->Draw()->vLine(bounds.x + pos, bottom, bottom - 512, 0xFFff3F3f);

	ctx->Draw()->hLine(bounds.x, bottom - 0.0333 * scale, bounds.x + width, 0xFF3f3Fff);
	ctx->Draw()->hLine(bounds.x, bottom - 0.0167 * scale, bounds.x + width, 0xFF3f3Fff);

	ctx->Flush();
	ctx->Begin();
	ctx->BindFontTexture();
	ctx->Draw()->SetFontScale(0.5f, 0.5f);
	ctx->Draw()->DrawText(ubuntu24, "33.3ms", bounds.x + width, bottom - 0.0333 * scale, 0xFF3f3Fff, ALIGN_BOTTOMLEFT | FLAG_DYNAMIC_ASCII);
	ctx->Draw()->DrawText(ubuntu24, "16.7ms", bounds.x + width, bottom - 0.0167 * scale, 0xFF3f3Fff, ALIGN_BOTTOMLEFT | FLAG_DYNAMIC_ASCII);
	ctx->Draw()->SetFontScale(1.0f, 1.0f);
	ctx->Flush();
	ctx->RebindTexture();
}

static void DrawFrameTiming(UIContext *ctx, const Bounds &bounds) {
	FontID ubuntu24("UBUNTU24");

	char statBuf[1024]{};

	ctx->Flush();
	ctx->BindFontTexture();
	ctx->Draw()->SetFontScale(0.5f, 0.5f);

	snprintf(statBuf, sizeof(statBuf),
		"Mode (interval): %s (%d)",
		Draw::PresentModeToString(g_frameTiming.presentMode),
		g_frameTiming.presentInterval);

	ctx->Draw()->DrawTextRect(ubuntu24, statBuf, bounds.x + 10, bounds.y + 50, bounds.w - 20, bounds.h - 30, 0xFFFFFFFF, FLAG_DYNAMIC_ASCII);

	for (int i = 0; i < 5; i++) {
		size_t curIndex = i + 6;
		size_t prevIndex = i + 7;

		FrameTimeData data = ctx->GetDrawContext()->FrameTimeHistory().Back(curIndex);
		FrameTimeData prevData = ctx->GetDrawContext()->FrameTimeHistory().Back(prevIndex);
		if (data.frameBegin == 0.0) {
			snprintf(statBuf, sizeof(statBuf), "(No frame time data)");
		} else {
			double stride = data.frameBegin - prevData.frameBegin;
			double fenceLatency_s = data.afterFenceWait - data.frameBegin;
			double submitLatency_s = data.firstSubmit - data.frameBegin;
			double queuePresentLatency_s = data.queuePresent - data.frameBegin;
			double actualPresentLatency_s = data.actualPresent - data.frameBegin;
			double presentMargin = data.presentMargin;
			double computedMargin = data.actualPresent - data.queuePresent;

			char presentStats[256] = "";
			if (data.actualPresent != 0.0) {
				snprintf(presentStats, sizeof(presentStats),
					"* Present: %0.1f ms\n"
					"* Margin: %0.1f ms\n"
					"* Margin(c): %0.1f ms\n",
					actualPresentLatency_s * 1000.0,
					presentMargin * 1000.0,
					computedMargin * 1000.0);
			}
			snprintf(statBuf, sizeof(statBuf),
				"* Stride: %0.1f (waits: %d)\n"
				"%llu: From start:\n"
				"* Past fence: %0.1f ms\n"
				"* Submit #1: %0.1f ms\n"
				"* Queue-p: %0.1f ms\n"
				"%s",
				stride * 1000.0,
				data.waitCount,
				(long long)data.frameId,
				fenceLatency_s * 1000.0,
				submitLatency_s * 1000.0,
				queuePresentLatency_s * 1000.0,
				presentStats
			);
		}
		ctx->Draw()->DrawTextRect(ubuntu24, statBuf, bounds.x + 10 + i * 150, bounds.y + 150, bounds.w - 20, bounds.h - 30, 0xFFFFFFFF, FLAG_DYNAMIC_ASCII);
	}
	ctx->Draw()->SetFontScale(1.0f, 1.0f);
	ctx->Flush();
	ctx->RebindTexture();
}

void DrawFramebufferList(UIContext *ctx, GPUDebugInterface *gpu, const Bounds &bounds) {
	if (!gpu) {
		return;
	}
	FontID ubuntu24("UBUNTU24");
	auto list = gpu->GetFramebufferList();
	ctx->Flush();
	ctx->BindFontTexture();
	ctx->Draw()->SetFontScale(0.7f, 0.7f);

	int i = 0;
	for (const VirtualFramebuffer *vfb : list) {
		char buf[512];
		snprintf(buf, sizeof(buf), "%08x (Z %08x): %dx%d (stride %d, %d)",
			vfb->fb_address, vfb->z_address, vfb->width, vfb->height, vfb->fb_stride, vfb->z_stride);
		ctx->Draw()->DrawTextRect(ubuntu24, buf, bounds.x + 10, bounds.y + 20 + i * 50, bounds.w - 20, bounds.h - 30, 0xFFFFFFFF, FLAG_DYNAMIC_ASCII);
		i++;
	}
	ctx->Flush();
}

void DrawControlMapperOverlay(UIContext *ctx, const Bounds &bounds, const ControlMapper &controlMapper) {
	DrawControlDebug(ctx, controlMapper, ctx->GetLayoutBounds());
}

void DrawDebugOverlay(UIContext *ctx, const Bounds &bounds, DebugOverlay overlay) {
	bool inGame = GetUIState() == UISTATE_INGAME;

	switch (overlay) {
	case DebugOverlay::DEBUG_STATS:
		if (inGame)
			DrawDebugStats(ctx, ctx->GetLayoutBounds());
		break;
	case DebugOverlay::FRAME_GRAPH:
		if (inGame)
			DrawFrameTimes(ctx, ctx->GetLayoutBounds());
		break;
	case DebugOverlay::FRAME_TIMING:
		DrawFrameTiming(ctx, ctx->GetLayoutBounds());
		break;
	case DebugOverlay::Audio:
		DrawAudioDebugStats(ctx, ctx->GetLayoutBounds());
		break;
#if !PPSSPP_PLATFORM(UWP) && !PPSSPP_PLATFORM(SWITCH)
	case DebugOverlay::GPU_PROFILE:
		if (g_Config.iGPUBackend == (int)GPUBackend::VULKAN || g_Config.iGPUBackend == (int)GPUBackend::OPENGL) {
			DrawGPUProfilerVis(ctx, gpu);
		}
		break;
	case DebugOverlay::GPU_ALLOCATOR:
		if (g_Config.iGPUBackend == (int)GPUBackend::VULKAN || g_Config.iGPUBackend == (int)GPUBackend::OPENGL) {
			DrawGPUMemoryVis(ctx, gpu);
		}
		break;
#endif
	case DebugOverlay::FRAMEBUFFER_LIST:
		if (inGame)
			DrawFramebufferList(ctx, gpu, bounds);
		break;
	default:
		break;
	}
}


static const char *CPUCoreAsString(int core) {
	switch (core) {
	case 0: return "Interpreter";
	case 1: return "JIT";
	case 2: return "IR Interpreter";
	case 3: return "JIT using IR";
	default: return "N/A";
	}
}

void DrawCrashDump(UIContext *ctx, const Path &gamePath) {
	const MIPSExceptionInfo &info = Core_GetExceptionInfo();

	auto sy = GetI18NCategory(I18NCat::SYSTEM);
	FontID ubuntu24("UBUNTU24");
	std::string discID = g_paramSFO.GetDiscID();

	std::string activeFlags = PSP_CoreParameter().compat.GetActiveFlagsString();
	if (activeFlags.empty()) {
		activeFlags = "(no compat flags active)";
	}

	int x = 20 + System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_LEFT);
	int y = 20 + System_GetPropertyFloat(SYSPROP_DISPLAY_SAFE_INSET_TOP);

	ctx->Flush();
	if (ctx->Draw()->GetFontAtlas()->getFont(ubuntu24))
		ctx->BindFontTexture();
	ctx->Draw()->SetFontScale(1.1f, 1.1f);
	ctx->Draw()->DrawTextShadow(ubuntu24, sy->T_cstr("Game crashed"), x, y, 0xFFFFFFFF);

	char statbuf[4096];
	char versionString[256];
	snprintf(versionString, sizeof(versionString), "%s", PPSSPP_GIT_VERSION);

	bool checkingISO = false;
	bool isoOK = false;

	char crcStr[50]{};
	if (Reporting::HasCRC(gamePath)) {
		u32 crc = Reporting::RetrieveCRC(gamePath);
		std::vector<GameDBInfo> dbInfos;
		if (g_gameDB.GetGameInfos(discID, &dbInfos)) {
			for (auto &dbInfo : dbInfos) {
				if (dbInfo.crc == crc) {
					isoOK = true;
				}
			}
		}
		snprintf(crcStr, sizeof(crcStr), "CRC: %08x %s\n", crc, isoOK ? "(Known good!)" : "(not identified)");
	} else {
		// Queue it for calculation, we want it!
		// It's OK to call this repeatedly until we have it, which is natural here.
		Reporting::QueueCRC(gamePath);
		checkingISO = true;
	}

	// TODO: Draw a lot more information. Full register set, and so on.

#ifdef _DEBUG
	char build[] = "debug";
#else
	char build[] = "release";
#endif

	std::string sysName = System_GetProperty(SYSPROP_NAME);
	int sysVersion = System_GetPropertyInt(SYSPROP_SYSTEMVERSION);

	// First column
	y += 65;

	int columnWidth = (ctx->GetBounds().w - x - 10) / 2;
	int height = ctx->GetBounds().h;

	ctx->PushScissor(Bounds(x, y, columnWidth, height));

	// INFO_LOG(Log::System, "DrawCrashDump (%d %d %d %d)", x, y, columnWidth, height);

	snprintf(statbuf, sizeof(statbuf), R"(%s
%s (%s)
%s (%s)
%s v%d (%s)
%s
)",
ExceptionTypeAsString(info.type),
discID.c_str(), g_paramSFO.GetValueString("TITLE").c_str(),
versionString, build,
sysName.c_str(), sysVersion, GetCompilerABI(),
crcStr
);

	ctx->Draw()->SetFontScale(.7f, .7f);
	ctx->Draw()->DrawTextShadow(ubuntu24, statbuf, x, y, 0xFFFFFFFF);
	y += 160;

	if (info.type == MIPSExceptionType::MEMORY) {
		snprintf(statbuf, sizeof(statbuf), R"(
Access: %s at %08x (sz: %d)
PC: %08x
%s)",
MemoryExceptionTypeAsString(info.memory_type),
info.address,
info.accessSize,
info.pc,
info.info.c_str());
		ctx->Draw()->DrawTextShadow(ubuntu24, statbuf, x, y, 0xFFFFFFFF);
		y += 180;
	} else if (info.type == MIPSExceptionType::BAD_EXEC_ADDR) {
		snprintf(statbuf, sizeof(statbuf), R"(
Destination: %s to %08x
PC: %08x
RA: %08x)",
ExecExceptionTypeAsString(info.exec_type),
info.address,
info.pc,
info.ra);
		ctx->Draw()->DrawTextShadow(ubuntu24, statbuf, x, y, 0xFFFFFFFF);
		y += 180;
	} else if (info.type == MIPSExceptionType::BREAK) {
		snprintf(statbuf, sizeof(statbuf), R"(
BREAK
PC: %08x
)", info.pc);
		ctx->Draw()->DrawTextShadow(ubuntu24, statbuf, x, y, 0xFFFFFFFF);
		y += 180;
	} else {
		snprintf(statbuf, sizeof(statbuf), R"(
Invalid / Unknown (%d)
)", (int)info.type);
		ctx->Draw()->DrawTextShadow(ubuntu24, statbuf, x, y, 0xFFFFFFFF);
		y += 180;
	}

	std::string kernelState = __KernelStateSummary();

	ctx->Draw()->DrawTextShadow(ubuntu24, kernelState.c_str(), x, y, 0xFFFFFFFF);

	y += 40;

	ctx->Draw()->SetFontScale(.5f, .5f);

	ctx->Draw()->DrawTextShadow(ubuntu24, info.stackTrace.c_str(), x, y, 0xFFFFFFFF);

	ctx->Draw()->SetFontScale(.7f, .7f);

	ctx->PopScissor();

	// Draw some additional stuff to the right, explaining why the background is purple, if it is.
	// Should try to be in sync with Reporting::IsSupported().

	std::string tips;
	if (CheatsInEffect()) {
		tips += "* Turn off cheats.\n";
	}
	if (HLEPlugins::HasEnabled()) {
		tips += "* Turn off plugins.\n";
	}
	if (g_Config.uJitDisableFlags) {
		tips += StringFromFormat("* Don't use JitDisableFlags: %08x\n", g_Config.uJitDisableFlags);
	}
	if (GetLockedCPUSpeedMhz()) {
		tips += "* Set CPU clock to default (0)\n";
	}
	if (checkingISO) {
		tips += "* (waiting for CRC...)\n";
	} else if (!isoOK) {  // TODO: Should check that it actually is an ISO and not a homebrew
		tips += "* Verify and possibly re-dump your ISO\n  (CRC not recognized)\n";
	}
	if (g_paramSFO.GetValueString("DISC_VERSION").empty()) {
		tips += "\n(DISC_VERSION is empty)\n";
	}
	if (!tips.empty()) {
		tips = "Things to try:\n" + tips;
	}

	x += columnWidth + 10;
	y = 85;
	snprintf(statbuf, sizeof(statbuf),
		"CPU Core: %s (flags: %08x)\n"
		"Locked CPU freq: %d MHz\n"
		"Cheats: %s, Plugins: %s\n%s\n\n%s",
		CPUCoreAsString(g_Config.iCpuCore), g_Config.uJitDisableFlags,
		GetLockedCPUSpeedMhz(),
		CheatsInEffect() ? "Y" : "N", HLEPlugins::HasEnabled() ? "Y" : "N", activeFlags.c_str(), tips.c_str());

	ctx->Draw()->DrawTextShadow(ubuntu24, statbuf, x, y, 0xFFFFFFFF);
	ctx->Flush();
	ctx->Draw()->SetFontScale(1.0f, 1.0f);
	ctx->RebindTexture();
}

void DrawFPS(UIContext *ctx, const Bounds &bounds) {
	FontID ubuntu24("UBUNTU24");
	float vps, fps, actual_fps;
	__DisplayGetFPS(&vps, &fps, &actual_fps);

	char fpsbuf[64];
	int lines_drawn = 0;

	ctx->Flush();
	ctx->BindFontTexture();
	ctx->Draw()->SetFontScale(0.7f, 0.7f);

	if ((g_Config.iShowStatusFlags & ((int)ShowStatusFlags::FPS_COUNTER | (int)ShowStatusFlags::SPEED_COUNTER)) == ((int)ShowStatusFlags::FPS_COUNTER | (int)ShowStatusFlags::SPEED_COUNTER)) {
		// Both at the same time gets a shorter formulation.
		snprintf(fpsbuf, sizeof(fpsbuf), "%.0f/%02.0f (%05.1f%%)", actual_fps, fps, vps / ((g_Config.iDisplayRefreshRate / 60.0f * 59.94f) / 100.0f));
	} else {
		if (g_Config.iShowStatusFlags & (int)ShowStatusFlags::FPS_COUNTER) {
			snprintf(fpsbuf, sizeof(fpsbuf), "%.1f FPS", actual_fps);
		} else if (g_Config.iShowStatusFlags & (int)ShowStatusFlags::SPEED_COUNTER) {
			snprintf(fpsbuf, sizeof(fpsbuf), "%.1f%%", vps / (59.94f / 100.0f));
		}
	}
	if(g_Config.iShowStatusFlags & ((int)ShowStatusFlags::FPS_COUNTER | (int)ShowStatusFlags::SPEED_COUNTER)) {
		ctx->Draw()->DrawTextShadow(ubuntu24, fpsbuf, bounds.x2() - 20, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
	}

	if (System_GetPropertyBool(SYSPROP_CAN_READ_BATTERY_PERCENTAGE)) {
		if (g_Config.iShowStatusFlags & (int)ShowStatusFlags::BATTERY_PERCENT) {
			const int battery = System_GetPropertyInt(SYSPROP_BATTERY_PERCENTAGE);
			char indicator[6];
			if      (battery < 11) { strcpy(indicator, "     "); }
			else if (battery < 31) { strcpy(indicator, "|    "); }
			else if (battery < 51) { strcpy(indicator, "||   "); }
			else if (battery < 71) { strcpy(indicator, "|||  "); }
			else if (battery < 91) { strcpy(indicator, "|||| "); }
			else                   { strcpy(indicator, "|||||"); }
			snprintf(fpsbuf, sizeof(fpsbuf), "%d[%s]", battery, indicator);
			ctx->Draw()->DrawTextShadow(ubuntu24, fpsbuf, bounds.x2() - 20, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		}
	}

	snprintf(fpsbuf, sizeof(fpsbuf), "%d", __DisplayGetNumVblanks());
	ctx->Draw()->DrawTextShadow(ubuntu24, fpsbuf, bounds.x2() - 20, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);

	/*
	ctx->Draw()->DrawTextShadow(ubuntu24, "\u26A1\U0001F5F2\u33C7\u32CF\u2007\u20AC\u2139\u2328", bounds.x2() - 20, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
	ctx->Draw()->DrawTextShadow(ubuntu24, "\u2121\u2122\u2116\u33CD\u3231", bounds.x2() - 20, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
	ctx->Draw()->DrawTextShadow(ubuntu24, "30/\u20079 (\u200798.7%)", bounds.x2() - 20, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
	*/

	if (g_Config.iShowStatusFlags & (int)ShowStatusFlags::BATTERY_PERCENT) {
		// U
		ctx->Draw()->DrawTextShadow(ubuntu24, "0020 \u0020\u0021\u0022\u0023\u0024\u0025\u0026\u0027\u0028\u0029\u002A\u002B\u002C\u002D\u002E\u002F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0030 \u0030\u0031\u0032\u0033\u0034\u0035\u0036\u0037\u0038\u0039\u003A\u003B\u003C\u003D\u003E\u003F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0040 \u0040\u0041\u0042\u0043\u0044\u0045\u0046\u0047\u0048\u0049\u004A\u004B\u004C\u004D\u004E\u004F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0050 \u0050\u0051\u0052\u0053\u0054\u0055\u0056\u0057\u0058\u0059\u005A\u005B\u005C\u005D\u005E\u005F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0060 \u0060\u0061\u0062\u0063\u0064\u0065\u0066\u0067\u0068\u0069\u006A\u006B\u006C\u006D\u006E\u006F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0070 \u0070\u0071\u0072\u0073\u0074\u0075\u0076\u0077\u0078\u0079\u007A\u007B\u007C\u007D\u007E\u007F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);

		// W
		ctx->Draw()->DrawTextShadow(ubuntu24, "00A0 \u00A0\u00A1\u00A2\u00A3\u00A4\u00A5\u00A6\u00A7\u00A8\u00A9\u00AA\u00AB\u00AC\u00AD\u00AE\u00AF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "00B0 \u00B0\u00B1\u00B2\u00B3\u00B4\u00B5\u00B6\u00B7\u00B8\u00B9\u00BA\u00BB\u00BC\u00BD\u00BE\u00BF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "00C0 \u00C0\u00C1\u00C2\u00C3\u00C4\u00C5\u00C6\u00C7\u00C8\u00C9\u00CA\u00CB\u00CC\u00CD\u00CE\u00CF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "00D0 \u00D0\u00D1\u00D2\u00D3\u00D4\u00D5\u00D6\u00D7\u00D8\u00D9\u00DA\u00DB\u00DC\u00DD\u00DE\u00DF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "00E0 \u00E0\u00E1\u00E2\u00E3\u00E4\u00E5\u00E6\u00E7\u00E8\u00E9\u00EA\u00EB\u00EC\u00ED\u00EE\u00EF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "00F0 \u00F0\u00F1\u00F2\u00F3\u00F4\u00F5\u00F6\u00F7\u00F8\u00F9\u00FA\u00FB\u00FC\u00FD\u00FE\u00FF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);

		// E
		ctx->Draw()->DrawTextShadow(ubuntu24, "0100 \u0100\u0101\u0102\u0103\u0104\u0105\u0106\u0107\u0108\u0109\u010A\u010B\u010C\u010D\u010E\u010F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0110 \u0110\u0111\u0112\u0113\u0114\u0115\u0116\u0117\u0118\u0119\u011A\u011B\u011C\u011D\u011E\u011F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0120 \u0120\u0121\u0122\u0123\u0124\u0125\u0126\u0127\u0128\u0129\u012A\u012B\u012C\u012D\u012E\u012F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0130 \u0130\u0131\u0132\u0133\u0134\u0135\u0136\u0137\u0138\u0139\u013A\u013B\u013C\u013D\u013E\u013F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0140 \u0140\u0141\u0142\u0143\u0144\u0145\u0146\u0147\u0148\u0149\u014A\u014B\u014C\u014D\u014E\u014F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0150 \u0150\u0151\u0152\u0153\u0154\u0155\u0156\u0157\u0158\u0159\u015A\u015B\u015C\u015D\u015E\u015F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0160 \u0160\u0161\u0162\u0163\u0164\u0165\u0166\u0167\u0168\u0169\u016A\u016B\u016C\u016D\u016E\u016F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0170 \u0170\u0171\u0172\u0173\u0174\u0175\u0176\u0177\u0178\u0179\u017A\u017B\u017C\u017D\u017E\u017F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
	} else {
		// R
		ctx->Draw()->DrawTextShadow(ubuntu24, "0400 \u0400\u0401\u0402\u0403\u0404\u0405\u0406\u0407\u0408\u0409\u040A\u040B\u040C\u040D\u040E\u040F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0410 \u0410\u0411\u0412\u0413\u0414\u0415\u0416\u0417\u0418\u0419\u041A\u041B\u041C\u041D\u041E\u041F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0420 \u0420\u0421\u0422\u0423\u0424\u0425\u0426\u0427\u0428\u0429\u042A\u042B\u042C\u042D\u042E\u042F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0430 \u0430\u0431\u0432\u0433\u0434\u0435\u0436\u0437\u0438\u0439\u043A\u043B\u043C\u043D\u043E\u043F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0440 \u0440\u0441\u0442\u0443\u0444\u0445\u0446\u0447\u0448\u0449\u044A\u044B\u044C\u044D\u044E\u044F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0450 \u0450\u0451\u0452\u0453\u0454\u0455\u0456\u0457\u0458\u0459\u045A\u045B\u045C\u045D\u045E\u045F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0460 \u0460\u0461\u0462\u0463\u0464\u0465\u0466\u0467\u0468\u0469\u046A\u046B\u046C\u046D\u046E\u046F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0470 \u0470\u0471\u0472\u0473\u0474\u0475\u0476\u0477\u0478\u0479\u047A\u047B\u047C\u047D\u047E\u047F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0480 \u0480\u0481\u0482\u0483\u0484\u0485\u0486\u0487\u0488\u0489\u048A\u048B\u048C\u048D\u048E\u048F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "0490 \u0490\u0491\u0492\u0493\u0494\u0495\u0496\u0497\u0498\u0499\u049A\u049B\u049C\u049D\u049E\u049F", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "04A0 \u04A0\u04A1\u04A2\u04A3\u04A4\u04A5\u04A6\u04A7\u04A8\u04A9\u04AA\u04AB\u04AC\u04AD\u04AE\u04AF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "04B0 \u04B0\u04B1\u04B2\u04B3\u04B4\u04B5\u04B6\u04B7\u04B8\u04B9\u04BA\u04BB\u04BC\u04BD\u04BE\u04BF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "04C0 \u04C0\u04C1\u04C2\u04C3\u04C4\u04C5\u04C6\u04C7\u04C8\u04C9\u04CA\u04CB\u04CC\u04CD\u04CE\u04CF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "04D0 \u04D0\u04D1\u04D2\u04D3\u04D4\u04D5\u04D6\u04D7\u04D8\u04D9\u04DA\u04DB\u04DC\u04DD\u04DE\u04DF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "04E0 \u04E0\u04E1\u04E2\u04E3\u04E4\u04E5\u04E6\u04E7\u04E8\u04E9\u04EA\u04EB\u04EC\u04ED\u04EE\u04EF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
		ctx->Draw()->DrawTextShadow(ubuntu24, "04F0 \u04F0\u04F1\u04F2\u04F3\u04F4\u04F5\u04F6\u04F7\u04F8\u04F9\u04FA\u04FB\u04FC\u04FD\u04FE\u04FF", bounds.x2() - 100, lines_drawn++ * 26 + 10, 0xFF3FFF3F, ALIGN_TOPRIGHT | FLAG_DYNAMIC_ASCII);
	}

	ctx->Draw()->SetFontScale(1.0f, 1.0f);
	ctx->Flush();
	ctx->RebindTexture();
}
