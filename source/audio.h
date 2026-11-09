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
void audio_stop_sfx(void);

void audio_update(float dt);

#endif
