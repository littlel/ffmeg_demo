extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/timestamp.h"
#include "libavformat/avio.h"
#include "libavutil/file.h"
}

/*
功能：Demo 实现从h264/265裸流文件读取视频，转封装后通过avio自定义写文件函数，完成对裸流视频的mp4封装
todo：实现从内存读取h264帧输入进行转封装
*/

FILE* fp_write;
/*[写文件]*/
int write_buffer(void* opaque, uint8_t* buf, int buf_size) {
	if (!feof(fp_write)) {
		int true_size = fwrite(buf, 1, buf_size, fp_write);
		return true_size;
	}
	else {
		return -1;
	}
}
/*[文件seek]*/
int64_t seek_buffer(void* opaque, int64_t offset, int whence)
{
	int64_t new_pos = 0;
	switch (whence)
	{
	case SEEK_SET:
		new_pos = offset;
		fsetpos(fp_write, &new_pos);
		break;
	case SEEK_CUR:
	{
		fpos_t position;
		fgetpos(fp_write, &position);
		new_pos = position + offset;
		break;
	}
	case SEEK_END: // 此处可能有问题
	{
		new_pos = ftell(fp_write);
	}
	break;
	default:
		return -1;
	}
	return new_pos;
}

int main(int argc, char** argv)
{
	//AV_CODEC_ID_H264
	AVOutputFormat* ofmt = NULL;
	AVFormatContext* ifmt_ctx = NULL, * ofmt_ctx = NULL;

	AVIOContext* avio_out = NULL;

	AVPacket pkt;
	const char* in_filename, * out_filename;
	int ret, i;
	int stream_index = 0;
	int* stream_mapping = NULL;
	int stream_mapping_size = 0;
	unsigned char* outbuffer = NULL;

	in_filename = "test.264";
	out_filename = "src0264.mp4";
	int m_frame_index = 0;

	fp_write = fopen(out_filename, "wb+"); //输出文件
	if (!fp_write)
	{
		printf("open file failed\n");
		return 0;
	}
	do
	{
		// 1. 打开输入
		// 1.1 读取文件头，获取封装格式相关信息
		if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
			printf("Could not open input file '%s'", in_filename);
			break;
		}
		// 1.2 解码一段数据，获取流相关信息
		if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
			printf("Failed to retrieve input stream information");
			break;
		}
		av_dump_format(ifmt_ctx, 0, in_filename, 0);

		// 2. 打开输出
		// 2.1 分配输出ctx	
		avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", NULL);
		if (!ofmt_ctx) {
			printf("Could not create output context\n");
			ret = AVERROR_UNKNOWN;
			break;
		}
		outbuffer = (unsigned char*)av_malloc(32768);
		/*转为ts等流式视频格式 只需要实现写函数，转mp4格式还需要实现seek函数*/
		avio_out = avio_alloc_context(outbuffer, 32768, 1, NULL, NULL, write_buffer, seek_buffer);

		ofmt_ctx->pb = avio_out;
		ofmt_ctx->flags = AVFMT_FLAG_CUSTOM_IO;

		stream_mapping_size = ifmt_ctx->nb_streams;
		stream_mapping = (int*)av_mallocz_array(stream_mapping_size, sizeof(*stream_mapping));
		if (!stream_mapping) {
			ret = AVERROR(ENOMEM);
			break;
		}
		ofmt = ofmt_ctx->oformat;

		for (i = 0; i < ifmt_ctx->nb_streams; i++) {
			AVStream* out_stream;
			AVStream* in_stream = ifmt_ctx->streams[i];
			AVCodecParameters* in_codecpar = in_stream->codecpar;

			if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
				in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
				in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
				stream_mapping[i] = -1;
				continue;
			}

			stream_mapping[i] = stream_index++;

			// 2.2 将一个新流(out_stream)添加到输出文件(ofmt_ctx)
			out_stream = avformat_new_stream(ofmt_ctx, NULL);
			if (!out_stream) {
				printf("Failed allocating output stream\n");
				ret = AVERROR_UNKNOWN;
				break;
			}

			// 2.3 将当前输入流中的参数拷贝到输出流中
			ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
			if (ret < 0) {
				printf("Failed to copy codec parameters\n");
				break;
			}
			out_stream->codecpar->codec_tag = 0;
		}
		av_dump_format(ofmt_ctx, 0, out_filename, 1);
		if (ret < 0)
			break;
		// 3. 数据处理
		// 3.1 写输出文件头
		ret = avformat_write_header(ofmt_ctx, NULL);
		if (ret < 0) {
			printf("Error occurred when opening output file\n");
			break;
		}

		while (1) {
			AVStream* in_stream, * out_stream;

			// 3.2 从输出流读取一个packet
			ret = av_read_frame(ifmt_ctx, &pkt);
			if (ret < 0)
				break;

			in_stream = ifmt_ctx->streams[pkt.stream_index];
			if (pkt.stream_index >= stream_mapping_size ||
				stream_mapping[pkt.stream_index] < 0) {
				av_packet_unref(&pkt);
				continue;
			}

			pkt.stream_index = stream_mapping[pkt.stream_index];
			out_stream = ofmt_ctx->streams[pkt.stream_index];

			/* copy packet */
			// 3.3 更新packet中的pts和dts
			// 关于AVStream.time_base的说明：
			// 输入：输入流中含有time_base，在avformat_find_stream_info()中可取到每个流中的time_base
			// 输出：avformat_write_header()会根据输出的封装格式确定每个流的time_base并写入文件中
			// AVPacket.pts和AVPacket.dts的单位是AVStream.time_base，不同的封装格式其AVStream.time_base不同
			// 所以输出文件中，每个packet需要根据输出封装格式重新计算pts和dts
			//av_packet_rescale_ts(&pkt, in_stream->time_base, out_stream->time_base);
			//从摄像头直接保存的h264文件，重新编码时得自己加时间戳，不然转换出来的是没有时间的

			//h264\265裸文件，重新编码时得自己加时间戳，不然转换出来的视频无时间信息会播放有问题
			if (pkt.pts == AV_NOPTS_VALUE) {
				//Write PTS
				AVRational time_base1 = in_stream->time_base;
				//Duration between 2 frames (us)
				int64_t calc_duration = (double)AV_TIME_BASE / av_q2d(in_stream->r_frame_rate);
				//Parameters
				pkt.pts = (double)(m_frame_index * calc_duration) / (double)(av_q2d(time_base1) * AV_TIME_BASE);
				pkt.dts = pkt.pts;
				pkt.duration = (double)calc_duration / (double)(av_q2d(time_base1) * AV_TIME_BASE);
			}
			pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
			pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
			pkt.pos = -1;

			// 3.4 将packet写入输出
			ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
			if (ret < 0) {
				printf("Error muxing packet\n");
				break;
			}
			av_packet_unref(&pkt);
			m_frame_index++;
		}

		// 3.5 写输出文件尾
		av_write_trailer(ofmt_ctx);
	} while (0);

	avformat_close_input(&ifmt_ctx);

	/* close output */
	if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
		avio_closep(&ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);

	av_freep(&stream_mapping);

	if (fp_write)
		fclose(fp_write);

	return 0;
}