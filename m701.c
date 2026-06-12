#define _GNU_SOURCE
#include <stdio.h>
#include <curses.h>  
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <locale.h> 
#include <wchar.h>
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define MAX 1024
#define P 512

char dir[P], *files[MAX], buf[P*2], playing[P], err[P];
int n, sel, st;
volatile int stop, alive, song_finished;
volatile int looping = 0;
volatile float volume = 1.0f;
volatile int paused = 0;
int vim_keys = 0;

ma_decoder *volatile g_dec = NULL;
ma_device *volatile g_dev = NULL;

void c(int y, const char *s);

int utf8_width(const char *s) {
    int len = 0;
    while (*s) {
        if ((*s & 0xc0) != 0x80) len++; 
        s++;
    }
    return len;
}

void resolve_path(const char *in, char *out, size_t out_len) {
    if (in[0] == '~') {
        const char *home = getenv("HOME");
        if (home) {
            if (in[1] == '/' || in[1] == '\0') {
                snprintf(out, out_len, "%s%s", home, in + 1);
                return;
            }
        }
    }
    snprintf(out, out_len, "%s", in);
}

void get_config_path(char *path, size_t len) {
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, len, "%s/.config/m701/config", home);
    } else {
        snprintf(path, len, ".m701_config");
    }
}

void load_config() {
    char path[P*2];
    get_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        dir[0] = '\0';
        vim_keys = 0;
        return;
    }
    char line[P*2];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "keybinds=", 9) == 0) {
            if (strcmp(line + 9, "vim") == 0) {
                vim_keys = 1;
            } else {
                vim_keys = 0;
            }
        } else if (strncmp(line, "dir=", 4) == 0) {
            snprintf(dir, sizeof(dir), "%s", line + 4);
        }
    }
    fclose(f);
}

void save_config() {
    char path[P*2];
    get_config_path(path, sizeof(path));
    const char *home = getenv("HOME");
    if (home) {
        char dir_path[P*2];
        snprintf(dir_path, sizeof(dir_path), "%s/.config", home);
        mkdir(dir_path, 0755);
        snprintf(dir_path, sizeof(dir_path), "%s/.config/m701", home);
        mkdir(dir_path, 0755);
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "keybinds=%s\n", vim_keys ? "vim" : "normal");
        fprintf(f, "dir=%s\n", dir);
        fclose(f);
    }
}

void show_keybinds() {
    erase();
    int y = LINES / 4;
    c(y++, "q - exit");
    c(y++, "r - toggle");
    c(y++, "enter / space - play / pause");
    c(y++, "plus / minus - volume up / down");
    c(y++, "left / right - fast forward / rewind");
    c(y++, "j / k - up / down");
    y++;
    c(y++, "any key to close");
    refresh();
    getch();
}

void config_menu() {
    int conf_sel = 0;
    int editing_dir = 0;
    curs_set(0);
    
    while (1) {
        erase();
        int y = LINES / 4;
        
        char dir_str[P*3];
        snprintf(dir_str, sizeof(dir_str), "dir - %s%s", dir, editing_dir ? "_" : "");
        if (conf_sel == 0) {
            attron(A_REVERSE);
            c(y++, dir_str);
            attroff(A_REVERSE);
        } else {
            c(y++, dir_str);
        }
        
        y += 2;
        
        char keys_str[64];
        snprintf(keys_str, sizeof(keys_str), "keys - %s", vim_keys ? "vim" : "normal");
        if (conf_sel == 1) {
            attron(A_REVERSE);
            c(y++, keys_str);
            attroff(A_REVERSE);
        } else {
            c(y++, keys_str);
        }
        
        y += 2;
        if (editing_dir) {
            c(y++, "enter to enter.");
        } else {
            c(y++, "q / esc to save.");
            if (conf_sel == 0) {
                c(y++, "enter to edit.");
            } else if (conf_sel == 1) {
                c(y++, "up/down - j/k vice versa.");
            }
        }
        
        refresh();
        int ch = getch();
        
        if (editing_dir) {
            int p = strlen(dir);
            if (ch == '\n' || ch == 27) {
                editing_dir = 0;
                curs_set(0);
            } else if (ch == 127 || ch == '\b' || ch == KEY_BACKSPACE) {
                if (p) {
                    while (p > 0 && (dir[--p] & 0xc0) == 0x80);
                    dir[p] = '\0';
                }
            } else if (p < P - 1 && ch >= 32 && ch < 127) {
                dir[p++] = ch;
                dir[p] = '\0';
            }
        } else {
            if (ch == 'q' || ch == 'Q' || ch == 27) {
                break;
            }
            if (ch == KEY_UP || ch == 'k') {
                conf_sel = 0;
            }
            if (ch == KEY_DOWN || ch == 'j') {
                conf_sel = 1;
            }
            if (ch == '\n' || ch == ' ') {
                if (conf_sel == 0) {
                    editing_dir = 1;
                    curs_set(1);
                } else if (conf_sel == 1) {
                    vim_keys = !vim_keys;
                }
            }
        }
    }
    save_config();
}

void c(int y, const char *s) { 
    int x = (COLS - utf8_width(s)) / 2; 
    mvprintw(y, x < 0 ? 0 : x, "%s", s); 
}

int audio(const char *f) {
    const char *e[] = {".mp3",".flac",".wav",0};
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
    char rdir[P*2];
    resolve_path(dir, rdir, sizeof(rdir));
    DIR *d = opendir(rdir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX) if (audio(e->d_name)) files[n++] = strdup(e->d_name);
    closedir(d);
}

void draw() {
    erase();
    int y = 0, vis = (LINES - 5) / 2;
    c(y++, dir); y++;
    if (sel < st) st = sel;
    if (sel >= st + vis) st = sel - vis + 1;
    for (int i = st; i < n && y < LINES - 3; i++) {
        if (i == sel) attron(A_REVERSE);
        c(y++, files[i]);
        if (i == sel) attroff(A_REVERSE);
    }
    if (alive && playing[0]) { snprintf(buf, sizeof(buf), "> %s [%d%%]", playing, (int)(volume * 100)); c(LINES - 7, buf); }
    if (err[0]) { attron(A_BOLD); c(LINES - 6, err); attroff(A_BOLD); }
    snprintf(buf, sizeof(buf), "q - exit | r - loop (%s)", looping ? "on" : "off");
    c(LINES - 2, buf);
    c(LINES - 1, "h - keybinds | F1 - config");
    refresh();
}

void stop_play() {
    if (!alive) return;
    stop = 1;
    while (alive) usleep(10000);
}

void data_callback(ma_device *dev, void *out, const void *in, ma_uint32 count) {
    ma_decoder *dec = (ma_decoder*)dev->pUserData;
    ma_uint64 read = 0;
    ma_decoder_read_pcm_frames(dec, out, count, &read);
    if (read < count) {
        if (looping) {
            ma_decoder_seek_to_pcm_frame(dec, 0);
            ma_uint64 read2 = 0;
            char* out_ptr = (char*)out + read * ma_get_bytes_per_frame(dec->outputFormat, dec->outputChannels);
            ma_decoder_read_pcm_frames(dec, out_ptr, count - read, &read2);
            read += read2;
            if (read < count) {
                memset((char*)out + read * ma_get_bytes_per_frame(dec->outputFormat, dec->outputChannels), 0, (count - read) * ma_get_bytes_per_frame(dec->outputFormat, dec->outputChannels));
            }
        } else {
            song_finished = 1;
            memset((char*)out + read * ma_get_bytes_per_frame(dec->outputFormat, dec->outputChannels), 0, (count - read) * ma_get_bytes_per_frame(dec->outputFormat, dec->outputChannels));
        }
    }
    short *samples = (short*)out;
    ma_uint32 sample_count = count * dec->outputChannels;
    for (ma_uint32 i = 0; i < sample_count; i++) {
        int val = (int)(samples[i] * volume);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        samples[i] = (short)val;
    }
}

void *play_thread(void *arg) {
    char *path = arg;
    err[0] = 0;
    alive = 1;
    stop = 0;
    song_finished = 0;
    paused = 0;
    ma_decoder dec;
    ma_decoder_config dec_cfg = ma_decoder_config_init(ma_format_s16, 2, 0);
    if (ma_decoder_init_file(path, &dec_cfg, &dec) != MA_SUCCESS) {
        snprintf(err, sizeof(err), "open");
        goto clean;
    }
    ma_device dev;
    ma_device_config dev_cfg = ma_device_config_init(ma_device_type_playback);
    dev_cfg.playback.format = dec.outputFormat;
    dev_cfg.playback.channels = dec.outputChannels;
    dev_cfg.sampleRate = dec.outputSampleRate;
    dev_cfg.dataCallback = data_callback;
    dev_cfg.pUserData = &dec;
    if (ma_device_init(NULL, &dev_cfg, &dev) != MA_SUCCESS) {
        snprintf(err, sizeof(err), "device");
        ma_decoder_uninit(&dec);
        goto clean;
    }
    g_dec = &dec;
    g_dev = &dev;
    if (ma_device_start(&dev) != MA_SUCCESS) {
        snprintf(err, sizeof(err), "start");
        g_dec = NULL;
        g_dev = NULL;
        ma_device_uninit(&dev);
        ma_decoder_uninit(&dec);
        goto clean;
    }
    while (!stop && !song_finished) usleep(10000);
    g_dec = NULL;
    g_dev = NULL;
    ma_device_uninit(&dev);
    ma_decoder_uninit(&dec);
clean:
    free(path);
    alive = 0;
    return NULL;
}

int main() {
    setlocale(LC_ALL, "");
    initscr(); cbreak(); noecho(); keypad(stdscr, 1);
    load_config();
    config_menu();
    scan();
    while (1) {
        draw();
        int ch = getch();
        if (ch == 'q' || ch == 'Q') { stop_play(); break; }
        if (ch == 'r' || ch == 'R') { looping = !looping; }
        if (ch == 'h' || ch == 'H') { show_keybinds(); }
        if (ch == KEY_F(1)) {
            stop_play();
            clear_files();
            config_menu();
            scan();
        }
        if ((ch == KEY_UP || (vim_keys && ch == 'k')) && sel > 0) sel--;
        if ((ch == KEY_DOWN || (vim_keys && ch == 'j')) && sel < n - 1) sel++;
        if (ch == KEY_LEFT) {
            if (alive && g_dec && g_dev) {
                ma_uint64 cursor = 0;
                ma_device_stop(g_dev);
                ma_decoder_get_cursor_in_pcm_frames(g_dec, &cursor);
                ma_uint64 step = 10 * g_dec->outputSampleRate;
                ma_uint64 target = (cursor > step) ? (cursor - step) : 0;
                ma_decoder_seek_to_pcm_frame(g_dec, target);
                if (!paused) ma_device_start(g_dev);
            }
        }
        if (ch == KEY_RIGHT || (vim_keys && ch == 'l')) {
            if (alive && g_dec && g_dev) {
                ma_uint64 cursor = 0;
                ma_device_stop(g_dev);
                ma_decoder_get_cursor_in_pcm_frames(g_dec, &cursor);
                ma_uint64 step = 10 * g_dec->outputSampleRate;
                ma_decoder_seek_to_pcm_frame(g_dec, cursor + step);
                if (!paused) ma_device_start(g_dev);
            }
        }
        if (ch == '+' || ch == '=') {
            volume += 0.05f;
            if (volume > 1.0f) volume = 1.0f;
        }
        if (ch == '-') {
            volume -= 0.05f;
            if (volume < 0.0f) volume = 0.0f;
        }
        if (ch == KEY_RESIZE) { }
        
        if ((ch == '\n' || ch == ' ') && n > 0) {
            if (alive && strcmp(playing, files[sel]) == 0) {
                if (paused) {
                    ma_device_start(g_dev);
                    paused = 0;
                } else {
                    ma_device_stop(g_dev);
                    paused = 1;
                }
            } else {
                stop_play();
                char *path = malloc(P*2);
                char rdir[P*2];
                resolve_path(dir, rdir, sizeof(rdir));
                int dl = strlen(rdir);
                snprintf(path, P*2, "%s%s%s", rdir, (dl && rdir[dl-1] == '/') ? "" : "/", files[sel]);
                snprintf(playing, sizeof(playing), "%s", files[sel]);
                pthread_t tid;
                pthread_create(&tid, NULL, play_thread, path);
                pthread_detach(tid);
            }
        }
    }
    clear_files();
    endwin();
    return 0;
}
