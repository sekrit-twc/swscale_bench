#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" {
  #include <libavutil/frame.h>
  #include <libavutil/pixdesc.h>
  #include <libavutil/pixfmt.h>
  #include <libswscale/swscale.h>
} // extern "C"

#include "argparse.h"
#include "timer.h"

namespace {;

int decode_pixfmt(const ArgparseOption *, void *out, int argc, char **argv)
{
	if (argc < 1)
		return -1;

	AVPixelFormat *pixfmt = static_cast<AVPixelFormat *>(out);
	AVPixelFormat format = av_get_pix_fmt(*argv);

	if (format == AV_PIX_FMT_NONE) {
		std::cerr << "bad pixel format\n";
		return -1;
	}

	*pixfmt = format;
	return 1;
}

void thread_target(AVPixelFormat pixfmt_in, AVPixelFormat pixfmt_out, int width_in, int width_out, int height_in, int height_out,
                   std::atomic_int *counter, std::atomic_int *error)
{
	SwsContext *sws = nullptr;
	AVFrame *src_frame = nullptr;
	AVFrame *dst_frame = nullptr;
	double params[] = { 4.0 };
	int flags = SWS_LANCZOS | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND | SWS_BITEXACT;
	int err = 1;

	if (!(sws = sws_getContext(width_in, height_in, pixfmt_in, width_out, height_out, pixfmt_out, flags, nullptr, nullptr, params)))
		goto fail;

	if (!(src_frame = av_frame_alloc()))
		goto fail;
	if (!(dst_frame = av_frame_alloc()))
		goto fail;

	src_frame->width = width_in;
	src_frame->height = height_in;
	src_frame->format = pixfmt_in;

	dst_frame->width = width_out;
	dst_frame->height = height_out;
	dst_frame->format = pixfmt_out;

	if ((err = av_frame_get_buffer(src_frame, 64)))
		goto fail;
	if ((err = av_frame_get_buffer(dst_frame, 64)))
		goto fail;

	while (true) {
		if ((*counter)-- <= 0)
			break;

		if (sws_scale(sws, src_frame->data, src_frame->linesize, 0, height_in, dst_frame->data, dst_frame->linesize) < 0)
			goto fail;
	}

	err = 0;
fail:
	if (err)
		error->store(err);

	av_frame_free(&src_frame);
	av_frame_free(&dst_frame);
	sws_freeContext(sws);
}

void execute(AVPixelFormat pixfmt_in, AVPixelFormat pixfmt_out, int width_in, int width_out, int height_in, int height_out,
             unsigned times, unsigned threads)
{
	unsigned thread_min = threads ? threads : 1;
	unsigned thread_max = threads ? threads : std::thread::hardware_concurrency();

	printf("%s @ %dx%d => %s @ %dx%d\n",
	       av_get_pix_fmt_name(pixfmt_in), width_in, height_in,
	       av_get_pix_fmt_name(pixfmt_out), width_out, height_out);

	for (unsigned n = thread_min; n <= thread_max; ++n) {
		std::vector<std::thread> thread_pool;
		std::atomic_int counter{ static_cast<int>(times * n) };
		std::atomic_int error{ 0 };
		Timer timer;

		timer.start();
		for (unsigned nn = 0; nn < n; ++nn) {
			thread_pool.emplace_back(thread_target, pixfmt_in, pixfmt_out, width_in, width_out, height_in, height_out,
			                         &counter, &error);
		}

		for (auto &th : thread_pool) {
			th.join();
		}
		timer.stop();

		if (error) {
			std::cerr << "failure: " << error << '\n';
			throw std::runtime_error{ "thread target failed" };
		}

		std::cout << '\n';
		std::cout << "threads:    " << n << '\n';
		std::cout << "iterations: " << times * n << '\n';
		std::cout << "fps:        " << (times * n) / timer.elapsed() << '\n';
	}
}


struct Arguments {
	AVPixelFormat pixfmt_in;
	AVPixelFormat pixfmt_out;
	int width_in;
	int height_in;
	int width_out;
	int height_out;
	unsigned times;
	unsigned threads;
};

const ArgparseOption program_switches[] = {
	{ OPTION_USER,     nullptr, "pixfmt-in",  offsetof(Arguments, pixfmt_in),  decode_pixfmt, "input pixel format"},
	{ OPTION_USER,     nullptr, "pixfmt-out", offsetof(Arguments, pixfmt_out), decode_pixfmt, "output pixel format"},
	{ OPTION_INTEGER,  nullptr, "width-in",   offsetof(Arguments, width_in),   nullptr, "input width" },
	{ OPTION_INTEGER,  nullptr, "height-in",  offsetof(Arguments, height_in),  nullptr, "input height" },
	{ OPTION_INTEGER,  nullptr, "width-out",  offsetof(Arguments, width_out),  nullptr, "output width" },
	{ OPTION_INTEGER,  nullptr, "height-out", offsetof(Arguments, height_out), nullptr, "output height" },
	{ OPTION_UINTEGER, nullptr, "times",      offsetof(Arguments, times),      nullptr, "number of benchmark cycles per thread" },
	{ OPTION_UINTEGER, nullptr, "threads",    offsetof(Arguments, threads),    nullptr, "number of threads" },
};

const ArgparseCommandLine program_def = {
	program_switches,
	sizeof(program_switches) / sizeof(program_switches[0]),
	nullptr,
	0,
	"swscale_bench",
	"benchmark swscale",
	nullptr
};

} // namespace


int main(int argc, char **argv)
{
	Arguments args{};
	int ret;

	args.pixfmt_in = AV_PIX_FMT_YUV420P10;
	args.pixfmt_out = AV_PIX_FMT_GBRP;
	args.width_in = 1280;
	args.height_in = 720;
	args.width_out = 1920;
	args.height_out = 1080;
	args.times = 100;

	if ((ret = argparse_parse(&program_def, &args, argc, argv)))
		return ret == ARGPARSE_HELP ? 0 : ret;

	try {
		execute(args.pixfmt_in, args.pixfmt_out, args.width_in, args.width_out, args.height_in, args.height_out,
		        args.times, args.threads);
	} catch (const std::runtime_error &e) {
		std::cerr << "runtime error: " << e.what() << '\n';
		return 1;
	}

	return 0;
}
