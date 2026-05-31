#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>

#define MAX 1024
#define P 512

char dir[P], *files[MAX], buf[P*2], playing[P], status[P];
int n, sel, st;

void c(int y, const char *s) { int x = (COLS - strlen(s)) / 2; mvprintw(y, x < 0 ? 0 : x, "%s", s); }

int audio(const char *f) {
    const char *e[] = {".mp3", ".flac", ".wav", ".ogg", ".m4a", NULL};
    const char *d = strrchr(f, '.');
    if (!d) return 0;
    for (int i = 0; e[i]; i++) if (!strcasecmp(d, e[i])) return 1;
    return 0;
}

void clear_files() {
    for (int i = 0; i < n; i++) free(files[i]);
    n = sel = st = 0;
}

void scan() {
    DIR *d = opendir(dir);
    if (!d) { clear(); c(LINES / 2, "failed. press any key."); refresh(); getch(); return; }
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX) if (audio(e->d_name)) files[n++] = strdup(e->d_name);
    closedir(d);
}

void draw() {
    clear();
    int y = 0, vis = (LINES - 5) / 2;
    c(y++, dir); y++;
    if (sel < st) st = sel;
    if (sel >= st + vis) st = sel - vis + 1;
    for (int i = st; i < n && y < LINES - 3; i++) {
        if (i == sel) attron(A_REVERSE);
        char *dot = strrchr(files[i], '.');
        int len = dot ? dot - files[i] : strlen(files[i]);
        snprintf(buf, sizeof(buf), "%.*s", len, files[i]);
        c(y++, buf);
        snprintf(buf, sizeof(buf), "(%s)", files[i]);
        c(y++, buf);
        if (i == sel) attroff(A_REVERSE);
    }
    if (playing[0]) {
        snprintf(buf, sizeof(buf), "> %s", playing);
        c(LINES - 7, buf);
    }
    if (status[0]) {
        attron(A_BOLD);
        c(LINES - 6, status);
        attroff(A_BOLD);
    }
    c(LINES - 2, "q - exit | r - reinit");
    c(LINES - 1, "enter - play");
    refresh();
}

void prompt() {
    const char *pr = "enter dir - ";
    int pl = strlen(pr), p = strlen(dir);
    curs_set(1);
    while (1) {
        clear();
        int sx = (COLS - pl - p) / 2;
        if (sx < 0) sx = 0;
        mvprintw(LINES / 2, sx, "%s", pr);
        mvprintw(LINES / 2, sx + pl, "%s", dir);
        move(LINES / 2, sx + pl + p);
        refresh();
        int ch = getch();
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') { if (p) dir[--p] = 0; }
        else if (p < P - 1 && ch >= 32 && ch < 127) { dir[p++] = ch; dir[p] = 0; }
        else if (ch == KEY_RESIZE) redrawwin(stdscr);
    }
    curs_set(0);
    FILE *out = fopen(".m", "w");
    if (out) { fprintf(out, "%s\n", dir); fclose(out); }
}

void load() {
    FILE *in = fopen(".m", "r");
    if (in) { fgets(dir, P, in); fclose(in); dir[strcspn(dir, "\n")] = 0; }
}

void play(const char *path) {
    av_log_set_level(AV_LOG_QUIET);
    if (access(path, R_OK) < 0) { snprintf(status, sizeof(status), "no access: %s", path); return; }
    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) {
        av_strerror(ret, buf, sizeof(buf));
        snprintf(status, sizeof(status), "open: %s", buf);
        return;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); snprintf(status, sizeof(status), "stream info failed"); return; }
    int stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { stream = i; break; }
    if (stream < 0) { avformat_close_input(&fmt); snprintf(status, sizeof(status), "no audio"); return; }
    AVCodecParameters *par = fmt->streams[stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { avformat_close_input(&fmt); snprintf(status, sizeof(status), "no codec"); return; }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) { avformat_close_input(&fmt); snprintf(status, sizeof(status), "ctx alloc failed"); return; }
    if (avcodec_parameters_to_context(ctx, par) < 0 || avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx); avformat_close_input(&fmt); snprintf(status, sizeof(status), "codec open failed"); return;
    }

    int in_ch = 2;
    int in_rate = par->sample_rate;
    #if LIBAVUTIL_VERSION_MAJOR >= 57
    if (par->ch_layout.nb_channels > 0) in_ch = par->ch_layout.nb_channels;
    #else
    if (par->channels > 0) in_ch = par->channels;
    #endif

    int out_ch = (in_ch > 2) ? 2 : in_ch;
    unsigned int out_rate = in_rate;

    snd_pcm_t *pcm = NULL;
    const char *devs[] = {"default", "hw:0,0", "hw:0", "plughw:0,0", NULL};
    for (int d = 0; devs[d]; d++) {
        if (snd_pcm_open(&pcm, devs[d], SND_PCM_STREAM_PLAYBACK, 0) >= 0) break;
    }
    if (!pcm) { snprintf(status, sizeof(status), "alsa open failed"); avcodec_free_context(&ctx); avformat_close_input(&fmt); return; }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16);
    snd_pcm_hw_params_set_channels(pcm, hw, out_ch);
    snd_pcm_hw_params_set_rate_near(pcm, hw, &out_rate, 0);
    if (snd_pcm_hw_params(pcm, hw) < 0) {
        snd_pcm_close(pcm); avcodec_free_context(&ctx); avformat_close_input(&fmt); snprintf(status, sizeof(status), "alsa params failed"); return;
    }
    int dir_dummy;
    snd_pcm_hw_params_get_rate(hw, &out_rate, &dir_dummy);

    SwrContext *swr = NULL;
    #if LIBAVUTIL_VERSION_MAJOR >= 57
    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout, in_ch);
    av_channel_layout_default(&out_layout, out_ch);
    swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_S16, (int)out_rate, &in_layout, ctx->sample_fmt, in_rate, 0, NULL);
    #else
    uint64_t in_layout = av_get_default_channel_layout(in_ch);
    uint64_t out_layout = av_get_default_channel_layout(out_ch);
    swr = swr_alloc_set_opts(NULL, out_layout, AV_SAMPLE_FMT_S16, (int)out_rate, in_layout, ctx->sample_fmt, in_rate, 0, NULL);
    #endif
    if (!swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        snd_pcm_close(pcm); avcodec_free_context(&ctx); avformat_close_input(&fmt); snprintf(status, sizeof(status), "resample failed"); return;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *outf = av_frame_alloc();
    outf->format = AV_SAMPLE_FMT_S16;
    outf->sample_rate = (int)out_rate;
    #if LIBAVUTIL_VERSION_MAJOR >= 57
    av_channel_layout_default(&outf->ch_layout, out_ch);
    #else
    outf->channel_layout = out_layout;
    outf->channels = out_ch;
    #endif

    int out_samples = av_rescale_rnd(4096, (int)out_rate, in_rate, AV_ROUND_UP);
    av_samples_alloc(outf->data, outf->linesize, out_ch, out_samples, AV_SAMPLE_FMT_S16, 0);

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream && avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                int out_count = av_rescale_rnd(swr_get_delay(swr, in_rate) + frame->nb_samples, (int)out_rate, in_rate, AV_ROUND_UP);
                if (out_count > out_samples) {
                    av_freep((void**)&outf->data[0]);
                    out_samples = out_count;
                    av_samples_alloc(outf->data, outf->linesize, out_ch, out_samples, AV_SAMPLE_FMT_S16, 0);
                }
                int conv = swr_convert(swr, outf->data, out_count, (const uint8_t**)frame->data, frame->nb_samples);
                if (conv > 0) {
                    snd_pcm_sframes_t w = snd_pcm_writei(pcm, outf->data[0], conv);
                    if (w < 0) snd_pcm_prepare(pcm);
                }
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frame) >= 0) {
        int out_count = av_rescale_rnd(swr_get_delay(swr, in_rate) + frame->nb_samples, (int)out_rate, in_rate, AV_ROUND_UP);
        if (out_count > out_samples) {
            av_freep((void**)&outf->data[0]);
            out_samples = out_count;
            av_samples_alloc(outf->data, outf->linesize, out_ch, out_samples, AV_SAMPLE_FMT_S16, 0);
        }
        int conv = swr_convert(swr, outf->data, out_count, (const uint8_t**)frame->data, frame->nb_samples);
        if (conv > 0) {
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, outf->data[0], conv);
            if (w < 0) snd_pcm_prepare(pcm);
        }
    }
    swr_convert(swr, outf->data, out_samples, NULL, 0);

    av_freep((void**)&outf->data[0]);
    av_frame_free(&outf);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    status[0] = 0;
}

int main() {
    initscr(); cbreak(); noecho(); keypad(stdscr, 1); curs_set(1);
    load();
    prompt();
    scan();
    while (1) {
        draw();
        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        if (ch == 'r' || ch == 'R') { status[0] = 0; clear_files(); prompt(); scan(); }
        if (ch == KEY_UP && sel > 0) sel--;
        if (ch == KEY_DOWN && sel < n - 1) sel++;
        if ((ch == '\n' || ch == '\r' || ch == KEY_ENTER) && n > 0) {
            char path[P*2];
            int dl = strlen(dir);
            snprintf(path, sizeof(path), "%s%s%s", dir, (dl > 0 && dir[dl-1] == '/') ? "" : "/", files[sel]);
            snprintf(playing, sizeof(playing), "%s", files[sel]);
            status[0] = 0;
            draw();
            play(path);
            playing[0] = 0;
        }
        if (ch == KEY_RESIZE) redrawwin(stdscr);
    }
    clear_files();
    endwin();
    return 0;
}
