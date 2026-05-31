#include <stdio.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>

#define MAX 1024
#define P 512

char dir[P], *files[MAX], buf[P*2], playing[P], err[P];
int n, sel, st;

void c(int y, const char *s) { int x = (COLS - strlen(s)) / 2; mvprintw(y, x < 0 ? 0 : x, "%s", s); }

int audio(const char *f) {
    const char *e[] = {".mp3",".flac",".wav",".ogg",".m4a",0};
    const char *d = strrchr(f, '.');
    if (!d) return 0;
    for (int i = 0; e[i]; i++) if (!strcasecmp(d, e[i])) return 1;
    return 0;
}

void clear_files() {
    while (n) free(files[--n]);
    sel = st = 0;
}

void scan() {
    DIR *d = opendir(dir);
    if (!d) return;
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
        c(y++, files[i]);
        if (i == sel) attroff(A_REVERSE);
    }
    if (playing[0]) { snprintf(buf, sizeof(buf), "> %s", playing); c(LINES - 7, buf); }
    if (err[0]) { attron(A_BOLD); c(LINES - 6, err); attroff(A_BOLD); }
    c(LINES - 2, "q - exit | r - reinit");
    c(LINES - 1, "enter - play");
    refresh();
}

void prompt() {
    const char *pr = "dir: ";
    int pl = strlen(pr), p = strlen(dir);
    curs_set(1);
    while (1) {
        clear();
        mvprintw(LINES / 2, (COLS - pl - p) / 2, "%s%s", pr, dir);
        move(LINES / 2, (COLS - pl - p) / 2 + pl + p);
        refresh();
        int ch = getch();
        if (ch == '\n') break;
        if (ch == 127 || ch == '\b') { if (p) dir[--p] = 0; }
        else if (p < P - 1 && ch >= 32 && ch < 127) { dir[p++] = ch; dir[p] = 0; }
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
    err[0] = 0;
    AVFormatContext *fmt = NULL;
    AVCodecContext *ctx = NULL;
    snd_pcm_t *pcm = NULL;
    SwrContext *swr = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    int16_t *out = NULL;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) { snprintf(err, sizeof(err), "open"); goto clean; }
    if (avformat_find_stream_info(fmt, NULL) < 0) { snprintf(err, sizeof(err), "info"); goto clean; }
    int si = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) { si = i; break; }
    if (si < 0) { snprintf(err, sizeof(err), "noaudio"); goto clean; }
    AVCodecParameters *par = fmt->streams[si]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { snprintf(err, sizeof(err), "codec"); goto clean; }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_parameters_to_context(ctx, par) < 0 || avcodec_open2(ctx, codec, NULL) < 0) {
        snprintf(err, sizeof(err), "ctx"); goto clean;
    }
    int rate = par->sample_rate;
    int ich = par->channels ? par->channels : 2;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) { snprintf(err, sizeof(err), "alsa"); goto clean; }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16);
    snd_pcm_hw_params_set_channels(pcm, hw, 2);
    unsigned int r = rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0);
    if (snd_pcm_hw_params(pcm, hw) < 0) { snprintf(err, sizeof(err), "aparams"); goto clean; }

    uint64_t in_layout = av_get_default_channel_layout(ich);
    uint64_t out_layout = av_get_default_channel_layout(2);
    swr = swr_alloc_set_opts(NULL, out_layout, AV_SAMPLE_FMT_S16, (int)r, in_layout, ctx->sample_fmt, rate, 0, NULL);
    if (!swr || swr_init(swr) < 0) { snprintf(err, sizeof(err), "swr"); goto clean; }

    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    out = malloc(48000 * 4);

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == si && avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                uint8_t *op = (uint8_t*)out;
                int conv = swr_convert(swr, &op, 48000, (const uint8_t**)frame->data, frame->nb_samples);
                if (conv > 0) {
                    snd_pcm_sframes_t w = snd_pcm_writei(pcm, out, conv);
                    if (w < 0) snd_pcm_prepare(pcm);
                }
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frame) >= 0) {
        uint8_t *op = (uint8_t*)out;
        int conv = swr_convert(swr, &op, 48000, (const uint8_t**)frame->data, frame->nb_samples);
        if (conv > 0) snd_pcm_writei(pcm, out, conv);
    }
    uint8_t *op = (uint8_t*)out;
    swr_convert(swr, &op, 48000, NULL, 0);
    snd_pcm_drain(pcm);

clean:
    free(out);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    swr_free(&swr);
    if (pcm) snd_pcm_close(pcm);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}

int main() {
    initscr(); cbreak(); noecho(); keypad(stdscr, 1); curs_set(1);
    load();
    prompt();
    scan();
    while (1) {
        draw();
        int ch = getch();
        if (ch == 'q') break;
        if (ch == 'r') { err[0] = 0; clear_files(); prompt(); scan(); }
        if (ch == KEY_UP && sel > 0) sel--;
        if (ch == KEY_DOWN && sel < n - 1) sel++;
        if (ch == '\n' && n > 0) {
            char path[P*2];
            int dl = strlen(dir);
            snprintf(path, sizeof(path), "%s%s%s", dir, (dl && dir[dl-1] == '/') ? "" : "/", files[sel]);
            snprintf(playing, sizeof(playing), "%s", files[sel]);
            draw();
            play(path);
            playing[0] = 0;
        }
    }
    clear_files();
    endwin();
    return 0;
}
