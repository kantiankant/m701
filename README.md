# m701
a dead simple, yet useable music player. 

# fyi

"Aim to create a system that is simple, transparent, and easy to pick up, without having to give up practicality and a rich feature set."

# who this is for 

- me
- people who want a plug and play but non-bloated tui music player

# compiling (void) 

```sudo xbps-install -S alsa-lib-devel ffmpeg-devel ncurses-devel gcc make```

```gcc m701.c -o m701 -pthread -lncurses -lasound -lavformat -lavcodec -lavutil -lswresample -lm```

