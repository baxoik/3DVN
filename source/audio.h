#ifndef AUDIO_H
#define AUDIO_H

/* Music streams from the SD card and loops; sound effects play from memory. */
void audio_init(void);
void audio_exit(void);
int audio_available(void);

/* Paths are full file paths. Returns 1 on success, else 0 with a short reason in err. */
int audio_play_music(const char *path, float fade_seconds, char *err, int err_size);
void audio_stop_music(float fade_seconds);
int audio_play_sfx(const char *path, char *err, int err_size);
/* A sound effect that repeats until "sfx stop": plays to loop_end seconds (0 = the end of the
   file), then continues at loop_start. Running loops keep their slot. */
int audio_play_sfx_loop(const char *path, int loop, float loop_start, float loop_end, char *err, int err_size);
void audio_stop_sfx(void);
/* 1 while any sound effect (including voice clips) is still playing. */
int audio_sfx_playing(void);

/* Volumes 0..1. Sounds under audio/v/ are voices and use voice; other sound effects
   follow master only. Applies to playing sounds at once. */
void audio_set_volumes(float master, float music, float voice);

void audio_update(float dt);

#endif
