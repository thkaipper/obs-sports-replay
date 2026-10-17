#pragma once
#include <obs-module.h>
#include <QString>
#ifdef _WIN32
#include <windows.h>
#endif

static bool sr_test_video()
{
	QString root = qEnvironmentVariable("SR_OBS_RUNTIME_DIR", "C:/Program Files/obs-studio");
#ifdef _WIN32
	QString bin = root + "/bin/64bit";
	AddDllDirectory(reinterpret_cast<LPCWSTR>(bin.utf16()));
#endif
	QByteArray data = (root + "/data/libobs/").toUtf8();
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
	obs_add_data_path(data.constData());
#ifdef _MSC_VER
#pragma warning(pop)
#endif
	QByteArray graphics = (root + "/bin/64bit/libobs-d3d11.dll").toUtf8();
	obs_video_info video = {};
	video.graphics_module = graphics.constData();
	video.fps_num = 30;
	video.fps_den = 1;
	video.base_width = video.base_height = video.output_width = video.output_height = 64;
	video.output_format = VIDEO_FORMAT_NV12;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_PARTIAL;
	return obs_reset_video(&video) == OBS_VIDEO_SUCCESS;
}
